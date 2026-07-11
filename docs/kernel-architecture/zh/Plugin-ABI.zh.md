# 插件 ABI

Photospider 支持操作插件和调度器插件。操作插件通过 host 提供的 registrar 扩展进程拥有的
`OpRegistry`。调度器插件提供 `IScheduler` 实现。

## 操作插件 ABI

操作插件导出一个带版本的 registrar 入口：

```cpp
extern "C" PLUGIN_API void register_photospider_ops_v1(
    ps::OperationPluginRegistrar* registrar);
```

加载器在加载动态库后调用该函数。Host 创建 `OperationPluginRegistrar`，把它传给插件，并把每一次注册转发到
进程唯一的 `OpRegistry`。操作插件在注册期间不得调用 `OpRegistry::instance()`，否则动态插件和静态
host 可能观察到不同的 registry singleton。

旧的无参数 `register_photospider_ops()` 不是受支持的兼容 ABI。它使用同一个 C 符号形态但没有签名标记，
因此加载器现在只解析 `register_photospider_ops_v1`，并拒绝旧插件，而不是猜测不兼容的函数指针类型。

支持的操作注册包括：

| 注册 | 含义 |
| --- | --- |
| HP monolithic | 全图 HP 实现。 |
| HP tiled | 基于 tile 的 HP 实现。 |
| RT tiled | 基于 tile 的 RT 实现。 |
| Dirty ROI propagator | 反向 ROI 传播。 |
| Forward ROI propagator | 下游 ROI 投影。 |
| Dependency LUT builder | 数据依赖空间依赖映射。 |
| Device implementation | CPU、Metal、CUDA 或未来后端实现元数据。 |

操作插件依赖公共 `ImageBuffer` 和 `NodeOutput` 契约。

## 操作插件 Shim 与链接方式

操作插件不会为了访问 registry 符号而链接宽泛的静态 `photospider` 产品。仓库内 operation plugin 通过
`OperationPluginRegistrar` 注册；标准插件 target 在需要 `ImageBuffer`/OpenCV adapter 转换函数等运行时
helper 符号时，链接窄边界 `photospider_operation_plugin_shim`。该 shim 明确不包含 `OpRegistry`、内置
operation 注册、plugin manager、plugin loader、graph、scheduler 或 compute-service 代码。

这个拆分支持静态 host 方向：

- 静态 Photospider 进程拥有一个 `OpRegistry` 和一个 operation `PluginManager`，由所有 embedded Host 共享。
- 动态 operation plugin 从 host 接收注册 callback，因此 registry mutation 始终发生在该进程拥有的实例中。
- `photospider_operation_plugin_shim` 只作为插件 callback 代码所需 buffer adapter 函数的共享运行时 helper 边界。
- 插件 callback object 和插件实例化的返回值内部状态仍可能指向插件代码，因此进程 owner 和复制值中的 lease
  必须保留插件库，直到这些状态全部销毁。

符号可见性规则：

- Operation plugin registrar entry 使用 `PLUGIN_API`，loader 只把
  `register_photospider_ops_v1` 视为受支持的 operation-plugin ABI 入口。
  任何其他外部可见的 callback helper 符号都不是 loader 入口或兼容性契约。
- Operation plugin target 会定义 `PHOTOSPIDER_PLUGIN_BUILD`，因此即使在
  Windows 上，`PHOTOSPIDER_API` 也保持为空。插件 callback 代码不得通过
  `ps::GraphError` 等公共 value contract 导入 backend library 符号；只有
  registrar 入口通过 `PLUGIN_API` 导出。
- Loader 解析精确的带版本符号名。
- Shim 导出插件 callback 所需的运行时 adapter helper 符号；Windows 上使用 `WINDOWS_EXPORT_ALL_SYMBOLS`。
- 这仍然是 C++ ABI 边界，因为 callback 使用 `std::function`、`NodeOutput`、`Node` 和 `OpMetadata`。
  在未来纯 C ABI 替代这些 callback 形态之前，编译器、标准库和 Photospider 头兼容性仍然是版本敏感的。

打算通过 `plugin_dirs` 作为当前可加载插件使用的 operation plugin，也必须显式注册 dirty 与
forward ROI propagator。Registry 仍提供 legacy identity fallback 作为迁移支持，但该 fallback
不是完整插件契约。逐像素图像操作可以注册 pass-through ROI 函数；带副作用的 monolithic
操作必须说明自己的副作用语义，并仍然注册显式 propagator，用来描述上游需求和下游受影响区域元数据。

标准示例插件遵守该规则：

| 插件 op | 执行形态 | ROI 契约 |
| --- | --- | --- |
| `image_process:invert` | HP monolithic 逐像素图像变换 | 显式 pass-through dirty 与 forward ROI。 |
| `image_process:threshold` | HP monolithic 逐像素图像变换 | 显式 pass-through dirty 与 forward ROI。 |
| `io:save` | HP monolithic 副作用 sink | 显式 pass-through planning metadata；执行阶段重写完整文件。 |
| `image_generator:perlin_noise_metal` | HP monolithic Metal generator | 显式 generator-local pass-through ROI metadata；未启用 tiled Metal 执行。 |

## 操作插件加载事务

加载单个 operation plugin 是覆盖 loader 全部可观察状态的强事务。在调用
`register_photospider_ops_v1` 之前，loader 会为目标 `OpRegistry`、operation-source map、
结构化 load result 和 retained-handle map 创建 staged copy。Host 提供的 registrar 指向 staged registry，
因此 plugin callback 在注册期间绝不会修改 active registry。Registration capture、previous-source
计算、restoration snapshot、result 聚合和 handle 插入也都只修改 staged state。

事务有三种结果：

- 如果 registrar 或任意后续 staging 步骤抛出 `std::bad_alloc`，原始异常会继续传播。Registry callback、
  source、diagnostic 和 retained handle 在逻辑上都与加载该候选插件之前逐项完全一致。
- 如果 registrar 抛出其他标准异常，loader 只提交该候选插件的结构化 diagnostic。任何 callback、source、
  restoration snapshot 或 handle 都不会变为 active。
- 当全部 staging allocation 成功后，commit 会先把候选库 swap 进 retained-handle map，再 swap
  source/result 状态，最后发布完整 registry。这些操作必须为 `noexcept`，不存在会分配内存的 rollback 路径。

候选库是 transaction object 第一个拥有的 member，因此最后析构。任何注册失败时，staged registry
callback object 及其捕获的 plugin-owned state 都会在动态库 unmap 之前销毁。成功时，retained handle
会先于包含 plugin callback 的 registry 变为可见。这两条顺序规则既防止失败路径中的析构调用进入已卸载库，
也防止成功路径中出现没有存活 handle 的 callback。

进程 manager 会串行化从完整 registry snapshot 到 publication 的整个区间。因此，直接 registry
registration 不会落在事务 copy 与最终 swap 之间后被覆盖丢失。Registry read 返回独立的 callback
snapshot，而不是借用指针；candidate filter 会在 registry lock 释放之后执行。如果 direct mutation 在
registrar staging 期间启动，它会等待 publication，然后应用到刚发布的 registry state；两项操作都能
完成，不会覆盖 direct update，也不会死锁。

ownership 的跟踪粒度低于 operation key。每次成功写入 legacy callback、metadata、HP/RT callback、
propagation callback、dependency builder、聚合 dependency flag 或 device implementation element，
都会得到稳定的 revision token。Plugin registration capture 只记录 registrar 实际写入的最终 token，
并把 predecessor snapshot 裁剪到这些被替换的 slot；append-only device predecessor 会继续留在 live
state，而不会复制进 restoration state。publication 后的 same-key direct mutation 会为它修改的 slot
取得新 token。Direct 与 plugin-owned slot 共存期间，source inspection 报告 `mixed`，不会继续把完整
key 归因于 plugin。

Live device implementation element 使用稳定、不可变的 owner，而不是把 `std::function` target 直接存进
会增长的 registry vector。新的 monolithic 或 tiled device value（包括其 plugin lease wrapper）会在获取
registry lock 前完整构造。加锁后，registration 只增长并发布 shared owner 及其平行 revision token。
Reader 在锁内保留一份一致的 owner 列表，只在释放 lock 后复制 callback target。第一个 CPU candidate 的
legacy HP compatibility slot 是持有同一 stable owner 的 forwarding bridge，不会复制原始 target。混合
plugin/direct 卸载期间，plugin-owned owner 会 swap 进预分配 retirement slot，后续 direct owner 则只会
swap 进已经清空的 gap；移除尾部时因此只会析构空 owner。任何已有或被移除的 device callback target
都不会在 registry lock 内被复制、移动、析构，也不会在锁内释放最后一个 library lease。新的稳定 value
或 compatibility bridge 构造失败发生在 key、callback 或 ownership 发布之前，并保持 registry 不变。

稳定所有权不等于执行 mutex。第一个 CPU device value 与其 HP compatibility bridge 会保留同一个 callback
target，scheduler 或 reader snapshot 也可能保留同一个逻辑 target。这些路径可以并发调用它。因此，callback
provider 必须保证 target 可重入，或自行同步其共享可变 state。Registry 只串行化 ownership mutation、
coherent snapshot capture、publication 与 unload；它绝不会持有 state lock 来串行化 callback execution。
Caller 不得因为 operation key、device 或 intent 相同，就推断 callback 只会单线程执行。

Direct replacement 同样遵循 manager-driven unload 之外的 retirement 规则。Replacement callback 会在
加锁前准备好，并与 active slot 交换；被替换的 callable 会留在参数局部 retirement value 中，直到
registry guard 已退出才析构。Whole-key unregister 会一起 extract legacy、metadata、implementation 与
ownership map node，再于 guard 外销毁移出的 value。因此 device implementation value 与其 revision
vector 始终保持平行；whole-key unregister 后的 direct device registration 不会继承 stale plugin token。

## 操作插件库生命周期

插件注册的操作回调可能指向该插件动态库内部的代码或 callable 对象。
`PluginManager::process_instance` 是 operation plugin source label、handle、restoration snapshot 与成功
加载顺序的唯一进程寿命 owner。所有 Kernel 和 embedded Host 都访问同一个 owner。销毁 Host 或 Kernel
不会卸载 operation plugin；任意 Host 执行显式 unload，所有 Host 都会观察到 registry/source 可见性变化。

一次成功加载会记录插件绝对路径、通过 host-provided registrar 注册或替换的 operation key、该 plugin
实际拥有的精确 per-slot revision、裁剪后的先前 registry/source state、预分配的空 callback-retirement
slot、RAII dynamic-library handle，以及单调递增的成功加载序号。生产低层 loader
要求不可伪造的 process-owner token，因此 caller 无法用第二套 source/handle/restoration map 向全局
registry 发布 callback。`PluginManager` 是唯一生产加载入口；不存在接收 caller source map 或在加载事务
提交后复制 manager state 的 legacy wrapper。

每个 registrar callback 都由共享 dynamic-library lease 包装。因而，已解析 callback snapshot 在显式
全局 unload 移除 registry entry 后仍可安全调用。Monolithic callback 返回的 `NodeOutput` 也会附着同一个
lease。该 lease 是第一个声明、最后销毁的 member；copy construction 会先保留 lease，再复制 payload
state；move construction 会通过 no-throw swap 转移完整 state。Copy/move assignment 会先暂存完整
replacement，再 swap 到位，并由 temporary 在释放旧 lease 前依次销毁旧
image/YAML/data/spatial/debug state；copy 失败时 destination 保持不变。因此，即使在显式全局 unload 后
复制、移动或覆盖 cached output，plugin 定义的 image/context deleter 执行时仍保持 library mapped。
这些 lease 不反向引用 manager 或 registry，因此不会形成 ownership cycle。

卸载只消费预先分配的 key、ownership token、snapshot 与 retirement slot。对于每个 scalar 或 device
element，它会比较 active revision 与 plugin publication token。匹配的 slot 从裁剪后的 predecessor
恢复，或 swap 进空 retirement storage；token 不同的后续 direct slot 继续保持 active。Device compaction
会让稳定 owner 只经过已经清空的 gap 进行 swap，并且只缩短由空 owner 构成的尾部，因此不依赖任何
`std::function` move 实现。随后可以 erase 空 registry value，而不会在 registry lock 内析构 plugin
callback state。Retired plugin record 会在释放该 lock 后析构。该路径不临时收集 key、不复制 callback、
不比较 callable，也不执行会分配的 rollback。因此，即使全局分配失败，`unload_all_plugins()` 仍是
`noexcept` 清理路径。

进程 owner 在静态 teardown 时有意不析构；显式 unload
定义插件清理语义，并避开与 `OpRegistry` 的静态析构顺序问题。

`unload_by_plugin_path()` 会先查找成功加载时记录的精确绝对 key。该 lookup 及后续清理不分配，
因此保留并传入所报告 source key 的 caller 可以获得与 unload-all 相同的清理保证。
相对或其他未归一化输入仍属于便利 API：`std::filesystem::absolute` 与 string 构造可能在清理开始前
分配。如果 normalization 失败，原始异常会在 registry、source、result 或 retained handle 状态发生
变化前传播。

卸载会先移除或恢复所有 callback 和 source 记录。随后在释放 registry lock 后销毁 retired callback
state；manager lock 对同线程递归，因此 plugin callback 或 DSO destructor 可以执行诊断性 registry/manager
read 而不会自死锁。只有在这些状态销毁后才释放 retained handle。`unload_all_plugins()` 严格按成功加载
序号逆序执行，
因此 built-in→旧 plugin→新 plugin 的覆盖链会依次回退为新 plugin、旧 plugin、built-in。按 path
排序不是合法卸载顺序，因为每个较新的 snapshot 都依赖紧邻的前一个实现。

如果旧插件已经被新插件 shadow，卸载旧插件时可能不会移除任何 active operation key。`PluginManager`
会使用同一套 slot token，只把旧插件拥有的 predecessor value 拼接到新插件 snapshot，再 retire 中间
callback。以后卸载新插件时可以恢复真正的
前驱，但绝不会恢复已 unmap 的中间库代码。该规则既适用于真实 built-in 或 host-registered sentinel
predecessor，也适用于原本不存在的 key；每个 retired plugin callback 都必须先于其自身 library unmap
销毁。

内置 callback 注册同样归进程 owner 管理。它最多执行一次，并且发生在 process-owner plugin 发布之前；
后续 Host seed 调用只对齐 source label，不能把内置实现重播到 active plugin replacement 之上。

## 调度器插件 ABI

调度器插件导出这些名称的 C 符号：

```text
ps_scheduler_plugin_get_count
ps_scheduler_plugin_get_name
ps_scheduler_plugin_get_description
ps_scheduler_plugin_create
ps_scheduler_plugin_destroy
ps_scheduler_plugin_get_version
```

必需导出：

| 导出 | 是否必需 | 含义 |
| --- | --- | --- |
| `get_count` | 是 | 插件中的调度器类型数量。 |
| `get_name` | 是 | 某索引处的类型名称。 |
| `create` | 是 | 为某类型创建调度器实例。返回对象必须同时实现 `IScheduler` 和 `SchedulerTaskRuntime`。 |
| `get_description` | 否 | 人类可读类型描述。 |
| `destroy` | 所有权所需 | 销毁插件创建的调度器实例。 |
| `get_version` | 否 | 插件版本字符串。 |

## 调度器插件加载事务

加载单个 scheduler plugin 是覆盖 scheduler type map、type metadata、retained-library map 与有序
load diagnostic 的强事务。打开 candidate 并解析导出后，loader 会为这四个容器创建局部 shadow copy。
Version、count、name 与 description callback 的结果、duplicate/conflict diagnostic、registered-type
bookkeeping 以及 candidate `PluginHandle` 都只记录在 shadow 中。因此，在 metadata 与 retained
library 准备完成前，任何 candidate type 都不会变为可见。

若任意 plugin callback、metadata copy、diagnostic construction 或 container allocation 抛出异常，
shadow state 会先于 candidate shared-library lifetime 释放而销毁。调用前精确的 type、metadata、handle
与 error prefix 会继续保持 active，原始异常 identity 原样传播，因此可以立即重试同一路径。完整 candidate
会在 loader mutex 下通过不抛异常的 container swap 发布，并首先 swap retained handle。Library-open
与缺少必需导出的失败仍返回 false 并产生一条 diagnostic，但该 diagnostic append 本身也经过 staging，
不会局部改变此前的 error sequence。

## 调度器实例运行时契约

当前调度器插件实例是以 `ps::IScheduler*` 返回的 C++ 对象，但 host 还要求同一个对象实现
`ps::SchedulerTaskRuntime`。加载器会在创建期间通过 `dynamic_cast` 验证这一点。一个插件即使导出了有效的
C 符号，并返回了 `IScheduler`，如果该对象没有同时实现 `SchedulerTaskRuntime`，它可以被发现，但在 host
尝试实例化该调度器类型时会被拒绝。

Host lifetime owner 是透明的 runtime wrapper。它会把 `available_devices()` 以及当前全部 callback
与借用 `TaskHandle` 的 single/batch submission method 直接转发给 plugin instance。它不能 fallback
到 `SchedulerTaskRuntime` base implementation，因为这可能替换 plugin 的 CPU/Metal device
inventory、把 atomic batch 拆成逐项 submission、改变 task ordering 或 exception identity。

这是当前过渡性 C++ ABI 的一部分。在长期纯 C ABI 替代该要求之前，插件作者应让具体调度器类同时继承这两个接口。

## 调度器实例所有权

由插件创建的调度器实例必须通过该插件的 destroy 函数销毁。加载器不得依赖默认 C++ deletion 销毁插件创建的实例。

该规则避免 allocator、runtime 和动态库边界问题。

非空 create 结果返回后，loader 会立刻建立一个不分配内存的栈上 guard，其中保存 raw instance、
destroy function 与 shared library lifetime。该 guard 会覆盖 `SchedulerTaskRuntime` 校验、host owner
的堆分配以及 type-name copy 构造。若 owner allocation、string copy 或其他构造步骤抛出异常，guard
会恰好一次调用 plugin destroy，并保证该调用返回前 library 一直保持映射。只有完整 host owner
构造成功后才转移所有权。

完整的 host owner 具有 `noexcept` destructor。析构会先清空 host 侧 raw/runtime pointer，
然后分别在两个独立 catch-all fence 后尝试 `shutdown()` 与 `detach()`。任一生命周期调用失败，
包括抛出 `std::bad_alloc`，都不能跳过后续阶段。之后 owner 会在第三个 no-throw ABI fence 后恰好
一次调用 plugin destroy export。若 destroy export 抛出异常，host 不会重试，因为它无法知道 plugin
是否已经结束或部分结束 object lifetime。Shared library lifetime 只会在这一次 destroy attempt 返回后
释放，因此 `shutdown`、`detach`、destroy 以及 plugin 侧 destructor code 都在 library 仍保持映射时执行。

这些 fence 只用于 destructor fallback 与 raw-owner 构造清理。显式 `attach`、`start`、`shutdown`
和 `detach` 调用仍会向 caller 保留并传播 plugin exception。这个区分既保持 public lifecycle contract
可观察，又防止 hostile plugin 在 host destruction 期间终止进程。

调度器插件库必须在由它创建的任何调度器实例仍可能存在期间保持加载。

## 当前 ABI 状态

当前调度器插件接口使用 C 符号名称，但返回 C++ `ps::IScheduler*`。这意味着二进制兼容性当前依赖兼容的 C++ ABI、编译器、标准库和 Photospider 接口版本。

这是过渡状态。长期方向是使用不透明 handle 或回调表的纯 C 调度器 ABI，使插件不依赖 C++ 二进制 ABI。

## 兼容性指南

- 操作插件应使用已发布的注册 API 和公共数据契约。
- 操作插件必须使用 `OperationPluginRegistrar` 和 `register_photospider_ops_v1`；无参数注册 ABI 不受支持。
- 操作插件不得仅为了共享 registry 状态而链接 `photospider`。只有当插件 callback 代码需要窄运行时 helper
  符号时，才使用 `photospider_operation_plugin_shim`。
- 调度器插件应将 `IScheduler` 和 `SchedulerTaskRuntime` ABI 兼容性都视为版本敏感。
- 调度器插件作者应实现 `ps_scheduler_plugin_destroy`。
- Host 应使用插件 destroy 销毁插件创建的调度器实例。
- 未来 C ABI 工作应作为单独的兼容性 change 完成。
