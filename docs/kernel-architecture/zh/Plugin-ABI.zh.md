# 插件 ABI

Photospider 支持操作插件和调度器插件。操作插件通过 host 提供的 registrar 扩展进程拥有的
`OpRegistry`。调度器插件通过数字握手提供 `IScheduler` 实现。两类接口都是当前临时 C++ ABI。
其 C linkage 入口只拦截 symbol identity 或 interface generation；它们不会让已接受的 value、callback
或 object 变成稳定 C data ABI。可安装的插件开发契约只位于
`include/photospider/{plugin,scheduler}`。

## 操作插件 ABI

操作插件导出一个带版本的 registrar 入口：

```cpp
extern "C" PHOTOSPIDER_OPERATION_PLUGIN_EXPORT void
register_photospider_ops_v2(
    ps::plugin::OperationPluginRegistrar* registrar);
```

加载器以 eager/local 方式打开候选库（POSIX 上使用 `RTLD_NOW | RTLD_LOCAL`），只解析这一精确符号，
并以借用 registrar 调用它。Registrar 写入 host 侧 shadow transaction；插件不会获得 `OpRegistry`
或其他可变 backend owner。

C linkage symbol 只保护精确 entrypoint lookup。Registrar table 与 operation contract 仍会让公共 C++
value、`std::function`、标准库 container、共享所有权与 exception 跨越 DSO boundary。因此，可加载的
operation plugin 必须使用匹配的 Photospider SDK，以及兼容 compiler、标准库、C++ ABI、
allocator/runtime、exception model 与 RTTI configuration。Version two 是临时边界：它不承诺纯 C
consumption、跨工具链 binary compatibility 或长期 ABI stability。

v1 `register_photospider_ops_v1` 和旧的无参数 `register_photospider_ops()` 都不是受支持的兼容 ABI。
只导出其中任一旧符号的 DSO 会被拒绝，且不会发布 callback。由于 v2 改变了 node、parameter、input、
output、ROI 与 dependency callback 类型，不能复用旧符号。

支持的操作注册包括：

| 注册 | 含义 |
| --- | --- |
| HP monolithic | 全图 HP 实现。 |
| HP tiled | 基于 tile 的 HP 实现。 |
| RT tiled | 基于 tile 的 RT 实现。 |
| Dirty ROI propagator | 反向 ROI 传播。 |
| Forward ROI propagator | 下游 ROI 投影。 |
| Dependency LUT builder | 数据依赖空间依赖映射。 |
| Device implementation | CPU、Metal、CUDA 或其他受支持的公共 `Device` capability。 |

Canonical registry identity 为 `type:subtype`。两个 segment 都必须非空，且都不能包含保留分隔符 `:`，
否则不同 pair 可能发生 identity collision。Public C++ registrar helper 还会在调用 `.c_str()` 前拒绝
embedded NUL byte，防止 raw ABI 截断改变 identity；host raw callback 会独立校验它实际可见的 C-string
segment。任何拒绝都发生在 candidate shadow transaction 内，不会发布 callback、source 或 handle。
每次 registration 还必须提供 non-empty callable。Typed C++ helper 会在进入 raw ABI 前拒绝 empty
`std::function`，host raw callback 也会再次校验，而不会信任 plugin wrapper。Loader 会把任一违规记录为
`InvalidParameter` candidate diagnostic，并保持 shadow 零发布。

Callback 边界与 host 实现解耦：

- `NodeView` 暴露 callback 周期内借用的 identity string，以及深拷贝拥有的有效
  `ParameterValue` tree；它不暴露 `Node`、`YAML::Node`、cache state 或 graph/runtime owner。
- `OperationInputView` 与 `OperationTileInputView` 只在 callback 期间借用不可变 image、named-data
  与 spatial snapshot。
- `OperationOutput` 拥有 image descriptor、named parameter value、spatial metadata 与 debug metadata；
  named value 会在 `ParameterMap` storage 与 private `NodeOutput` 之间直接复制或移动；host
  在附着 private DSO lease 前验证完整 output。
- `RoiContext` 暴露按输入顺序排列的 `InputEdgeView` topology snapshot；forward ROI callback 能识别
  active edge，dependency builder 返回 host 在缓存前验证的 owned `DependencyLutSnapshot`。
- `ParameterTypeError` 报告 plugin code 内明确的 `ParameterValue` alternative mismatch。
  Document conversion 在 Graph publication 前已经完成；callback preparation 只会在复制 owned
  snapshot 或分配 storage 时失败。

Callback entry 前的 host snapshot preparation 与成功返回后的 output validation 位于 plugin exception
fence 之外，并保留各自的 host-owned type。实际 plugin invocation 会保留显式 DSO lease，并在该 lease
可能释放前规范化每个 plugin-origin
exception：plugin `std::bad_alloc` 变成新的 host `std::bad_alloc`；plugin `GraphError` 变成保留同一固定宽度
code/message 的 host copy；`std::invalid_argument` 映射到 `GraphErrc::InvalidParameter`；其他 standard
或 unknown failure 映射到 `GraphErrc::ComputeError`。Plugin exception object 会在 lease 下完成检查与
销毁，因此其 identity 与 DSO-defined dynamic type 都不会到达 host。`GraphErrc` 使用固定 `uint32_t`
representation 与显式 `1..9` 数值。

## 操作 SDK Target 与链接方式

操作插件不会为了访问 registry 符号而链接宽泛的静态 `photospider` 产品。仓库内 operation plugin 通过
`OperationPluginRegistrar` 注册。普通插件请求 `operation_sdk` package component，并且只链接
`Photospider::operation_sdk`。该 interface target 提供安装头，并传递链接
`Photospider::operation_runtime`；后者的静态归档实现公共 image-buffer factory，不反向链接 SDK，
也不要求外部 package。

OpenCV 是显式 opt-in。使用 `photospider/plugin/opencv_adapter.hpp` 的插件额外请求并链接
`Photospider::operation_opencv`。该 target 拥有 adapter 实现，只发现 OpenCV `core`，不会带入
`imgproc`、`imgcodecs` 或 `videoio`。具体插件若算法需要其他 OpenCV module，必须自行声明。

通用 `ImageBuffer::context` 继续是 backend-specific opaque value。Public OpenCV adapter 只解释带非空
`data` 的 `Device::CPU` descriptor；它会拒绝非 CPU 或 context-only descriptor，而不会把任意 backend
resource cast 成 OpenCV object。Host dirty staging 会深拷 CPU data；非 CPU descriptor 在 tiled write 需要
CPU staging 前只做不可变共享；monolithic output 视为全量替换。Downsample planning 对非 CPU HP
descriptor 及其完整 extent 执行明确的 backend-preserving passthrough；它不会伪造低分辨率 pixel 或虚假的
缩小 extent。对于没有匹配 device adapter 的 descriptor，cache 与 metrics pixel inspection 会跳过。

这个拆分支持静态 host 方向：

- 静态 Photospider 进程拥有一个 `OpRegistry` 和一个 operation `PluginManager`，由所有 embedded Host 共享。
- 动态 operation plugin 从 host 接收注册 callback，因此 registry mutation 始终发生在该进程拥有的实例中。
- `Photospider::operation_runtime` 只包含 value-factory 实现，不包含 registry、loader、graph、scheduler
  或 compute state。
- 插件 callback object 和插件实例化的返回值内部状态仍可能指向插件代码，因此进程 owner 和复制值中的 lease
  必须保留插件库，直到这些状态全部销毁。

符号可见性规则：

- Operation registrar entry 使用 `PHOTOSPIDER_OPERATION_PLUGIN_EXPORT`，loader 只把
  `register_photospider_ops_v2` 视为 operation-plugin ABI 入口。
  任何其他外部可见的 callback helper 符号都不是 loader 入口或兼容性契约。
- Operation plugin target 定义 `PHOTOSPIDER_PLUGIN_BUILD`，从而在 Windows 导出 registrar，
  并在受支持的 POSIX 工具链上选择 default visibility。
- Loader 解析精确的带版本符号名。
- 这仍然是 C++ ABI 边界，因为 callback 使用 `std::function`、标准库 container 与 public C++ value。
  编译器、标准库、exception model、RTTI 设置与 Photospider SDK 兼容性仍然是版本敏感的。
  当前 ABI 不承诺跨工具链或纯 C 兼容性。

打算通过 `plugin_dirs` 作为当前可加载插件使用的 operation plugin，也必须显式注册 dirty 与
forward ROI propagator。Registry 仍提供 identity compatibility fallback，但该 fallback
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
`register_photospider_ops_v2` 之前，loader 会为目标 `OpRegistry`、operation-source map、
结构化 load result 和 retained-handle map 创建 staged copy。Host 提供的 registrar 指向 staged registry，
因此 plugin callback 在注册期间绝不会修改 active registry。Registration capture、previous-source
计算、restoration snapshot、result 聚合和 handle 插入也都只修改 staged state。

事务有三种结果：

- 如果 registrar 抛出 `std::bad_alloc`，plugin exception 会在 candidate lease 下完成检查与销毁，并传播
  新的 host `std::bad_alloc`。如果后续 host staging step 抛出 `std::bad_alloc`，该已经 host-owned 的异常
  会直接传播。Registry callback、source、diagnostic 和 retained handle 在逻辑上都与加载该候选插件之前
  逐项完全一致。
- 如果 registrar 抛出其他标准异常，loader 只提交该候选插件的结构化 diagnostic。任何 callback、source、
  restoration snapshot 或 handle 都不会变为 active；plugin exception 会先在 lease 下销毁。
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

仓库自有 CPU OpenCV provider 会用不可变 input、callback-local 或 task-owned `cv::Mat` state，
以及不使用进程范围的外层 operation mutex 来实现该契约。Builtin 注册会在 callback 发布前把 OpenCV
内部 CPU threading 固定为一，因此外层并行由 scheduler grant 拥有。真实共享 backend state 仍要求
provider-local 同步：Metal Perlin 的 DSO mutex 只保护其共享 Metal lifecycle。
[ADR 0004](../../adr/zh/0004-opencv-cpu-operations-are-reentrant-provider-work.zh.md)记录了该决策
及其 accounting 边界。

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
全局 unload 移除 registry entry 后仍可安全调用。Monolithic callback 的 public value 转成 host-private
`NodeOutput` 后也会附着同一个 lease。该 lease 是第一个声明、最后销毁的 member；copy construction
会先保留 lease，再复制 payload
state；move construction 会通过 no-throw swap 转移完整 state。Copy/move assignment 会先暂存完整
replacement，再 swap 到位，并由 temporary 在释放旧 lease 前依次销毁旧
image/ParameterValue/spatial/debug state；copy 失败时 destination 保持不变。因此，即使在显式全局 unload 后
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

调度器插件显式定义全部七个必需 C export：

```text
ps_scheduler_plugin_get_abi_version
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
| `get_abi_version` | 是，且为第一道 gate | 不抛异常的数字握手，签名是 `uint32_t() noexcept`；必须返回当前值为 `2` 的 `PS_SCHEDULER_PLUGIN_ABI_VERSION`。ABI v1 会被拒绝，且没有兼容路径。 |
| `get_count` | 是 | 插件中的调度器类型数量；必须至少为一。 |
| `get_name` | 是 | Candidate staging 期间保持稳定、由 DSO 拥有的类型名称；小于 count 的每个索引都必须返回非空指针、非空文本，随后由 host 复制。 |
| `get_description` | 是 | 某索引处的人类可读类型描述。 |
| `create` | 是 | 使用解析后的 `[1,8]` `num_workers` grant 为某类型创建调度器实例；`IScheduler` 已公开继承 `SchedulerTaskRuntime`。 |
| `destroy` | 是 | 销毁插件创建的调度器实例。 |
| `get_version` | 是 | 人类可读 implementation version；只调用一次并缓存，不作为兼容性 gate。 |

Count、index 与 worker-count 都使用固定宽度 `uint32_t`。可安装 SDK 提供 ABI constant、typed
function-pointer alias 与 `PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT`，但有意不提供 declaration macro 或
single-scheduler implementation macro。每个 DSO 必须显式声明所有 export，使生命周期与 visibility
契约保持可见。

ABI v2 把 worker-count 参数从语义未完整定义的配置值改为解析后的非零 hard grant。Host 会在调用
`create` 前验证 `[1,8]`，在 concrete instance 整个生命周期内预留完整授权，并且绝不会把 automatic
零 sentinel 传过该 ABI。Plugin 可以少拥有 worker thread，但不得超过授权。这是受信任的 in-process
contract：admission ledger 无法 sandbox 恶意 DSO，也无法阻止它在 load 期间或 returned scheduler
instance 之外创建未申报 thread。

## 调度器插件加载事务

加载单个 scheduler plugin 是覆盖 scheduler type map、type metadata、retained-library map 与有序
load diagnostic 的强事务。Loader 在 POSIX 上以 `RTLD_NOW | RTLD_LOCAL` 打开 candidate，首先且只
解析并调用 `ps_scheduler_plugin_get_abi_version`；只有它与 SDK 数字精确相等，才会解析其他 export。
缺少或不匹配 handshake 时，loader 释放 candidate 并记录结构化 diagnostic；不会调用 implementation
version、count、name、description、create 或 destroy，也不会发布 candidate state。特别是，ABI v1
library 只会执行数字 handshake；不会发布 v1 adapter、forwarding owner 或兼容 registration。

对于兼容 candidate，loader 会为四个容器创建局部 shadow copy。只调用一次的 implementation-version
结果，以及 count、name、description callback 结果、duplicate/conflict diagnostic、registered-type
bookkeeping 和 candidate `PluginHandle` 都只记录在 shadow 中。因此，在缓存 metadata 与 retained
library 准备完成前，任何 candidate type 都不会变为可见。

即使 candidate 兼容，只要 count 为零、任一范围内 name 是空指针或空字符串，或者全部有效 name 都与
已有 type 冲突，它仍会被拒绝。该拒绝会丢弃所有 candidate shadow、释放 candidate library，并在不改变
既有 diagnostic prefix 的前提下追加一条结构化 diagnostic。只要至少存在一个有效且不冲突的 type，冲突
仍然是可恢复的：该 type、对应 metadata、retained handle 与 staged conflict diagnostic 会一起提交。

若 discovery callback 抛出异常，loader 会在 candidate lease 存活期间应用与 operation callback fence
相同的 host-owned mapping。若 host metadata copy、diagnostic construction 或 container allocation 抛出
异常，该 host-owned exception 会保留其 type。两种情况下 shadow state 都会先于 candidate shared-library
lifetime 释放而销毁；调用前精确的 type、metadata、handle 与 error prefix 会继续保持 active，并且可以
立即重试同一路径。完整 candidate 会在 loader mutex 下通过不抛异常的 container swap 发布，并首先 swap
retained handle。Library-open 与缺少必需导出的失败仍返回 false 并产生一条 diagnostic，但该 diagnostic
append 本身也经过 staging，不会局部改变此前的 error sequence。

## 调度器实例运行时契约

调度器插件实例是以 `ps::IScheduler*` 返回的 C++ 对象。`IScheduler` 公开继承
`SchedulerTaskRuntime`，因此一个对象只有一套 lifecycle/runtime interface，创建阶段不执行跨 DSO
runtime type discovery。

`IScheduler::attach` 接收借用的 public `SchedulerHostContext&`，绝不接收 `GraphRuntime`。Context 只
暴露 device-capability query、task worker/epoch context set/clear 与 trace publication；其 protected
destructor 防止插件删除 host owner。Host 从 attach 成功起直至 shutdown、detach 都保证 context 存活，
scheduler 在 detach 时清空所有保留的 context pointer。因此 built-in 与 plugin scheduler 能保留 TLS
metrics 和 trace attribution，而无需访问 graph ownership 或 native Metal handle。

Host lifetime owner 是透明的 runtime wrapper。它会把 `available_devices()`、initial/worker-ready
借用 `TaskHandle` batch、any-thread callback submission、completion wait、first-exception publication、
completion-counter mutation 与 trace publication 全部直接转发给 plugin instance。两个 handle-batch
method 都是 pure virtual，因此 SDK 不能通过 base fallback 把 atomic batch 拆成逐项 submission，进而
改变 ordering。每个返回的 device 都会在进入 host planning 前按 CPU、Metal、CUDA 与 ASIC/NPU 校验；
unknown enumerator 会变成 host-owned `GraphError(InvalidParameter)`。`TaskExecutor` 使用 protected
virtual destructor，因此 plugin 无法通过借用的 base pointer 删除 dispatcher-owned executor。

Plugin discovery、create、lifecycle 与 runtime failure 会在显式 scheduler DSO lease 存活期间，使用与
operation callback 相同的 host-owned exception mapping。Host task 采用不同策略：在 `TaskHandle`
executor、any-thread callback 或非空 `set_exception` value 进入 plugin code 前，owner 会预分配
append-only identity slot。Relay 会在不分配内存的情况下记录原始 `exception_ptr`，并使用裸 `throw;`；
因此 plugin code 观察到原始 dynamic type、message 与 object。同一 pointer 之后再次出现时，host 会在
plugin-failure normalization 前识别它，并精确重抛。记录与查找不分配内存，任何 plugin call 都不会持有
registry guard；rejected admission 只把对应 slot 标记为 inactive，matching wait 会清除全部 slot。Registry
clear 会在 guard 内 swap storage，随后才销毁 retired exception object，因此 exception destructor 可以
重入 scheduler API 而不发生 deadlock。

这是当前过渡性 C++ ABI 的一部分。在另行版本化的纯 C ABI 替代该边界之前，插件作者只需继承
`IScheduler`，并实现其继承得到的 runtime operation。

创建 instance 时，plugin 会收到已经 planning 并 reservation 的精确 resolved grant。Automatic
hardware detection 此时已经产生一到八的值，显式请求则保持精确。仓库 example 也会自行校验相同
范围：CPU 与 heterogeneous example 精确拥有 grant 数量的 worker，serial example 不拥有 worker
thread，但仍按完整 plugin grant 计费。内置 GPU scheduler 独立的 grant 加一个 device-worker 计费不
属于 plugin ABI；plugin 没有未申报的额外 device-worker allowance。

## 调度器实例所有权

由插件创建的调度器实例必须通过该插件的 destroy 函数销毁。加载器不得依赖默认 C++ deletion 销毁插件创建的实例。

该规则避免 allocator、runtime 和动态库边界问题。

非空 create 结果返回后，loader 会立刻建立一个不分配内存的栈上 guard，其中保存 raw instance、
destroy function 与 shared library lifetime。该 guard 会覆盖 host owner 的堆分配以及 type-name
copy 构造。若 owner allocation、string copy 或其他构造步骤抛出异常，guard
会恰好一次调用 plugin destroy，并保证该调用返回前 library 一直保持映射。只有完整 host owner
构造成功后才转移所有权。

完整的 host owner 具有 `noexcept` destructor。析构会先清空 host 侧 raw/runtime pointer，
然后分别在两个独立 catch-all fence 后尝试 `shutdown()` 与 `detach()`。任一生命周期调用失败，
包括抛出 `std::bad_alloc`，都不能跳过后续阶段。之后 owner 会在第三个 no-throw ABI fence 后恰好
一次调用 plugin destroy export。若 destroy export 抛出异常，host 不会重试，因为它无法知道 plugin
是否已经结束或部分结束 object lifetime。Shared library lifetime 只会在这一次 destroy attempt 返回后
释放，因此 `shutdown`、`detach`、destroy 以及 plugin 侧 destructor code 都在 library 仍保持映射时执行。

这些 catch-all suppression fence 只用于 destructor fallback 与 raw-owner 构造清理。显式 `attach`、
`start`、`shutdown` 和 `detach` 调用仍保持可观察，但 plugin-origin failure 会使用上述 host-owned mapping，
不会导出 DSO exception object。这个区分既保持 public lifecycle contract 可观察，也防止未映射 dynamic
type 或 hostile cleanup exception 到达 host。

每次显式 `attach()` 或 `start()` 尝试之前，owner 都会先把对应 detach 或 shutdown fallback 标记为
必需；该标记发生在控制流进入 plugin 之前。若重复 attach/start 先发布局部状态、随后抛出异常，之前
成功完成的 detach/shutdown 不能让 destructor 跳过本次失败重试所需的 cleanup。

调度器插件库必须在由它创建的任何调度器实例仍可能存在期间保持加载。

Factory construction 会用单独的 `ReservationOwnedScheduler` 包装完成的 plugin owner。该 outer owner
会在 create、attach、start 与全部 runtime call 之后继续保留精确 grant。`GraphRuntime` 仍负责显式
shutdown 与 detach。销毁时，outer owner 会先销毁完整 plugin owner——后者在 DSO 保持 mapped 时执行
带 fence 的 lifecycle 与匹配 destroy export——随后才释放进程 reservation。Candidate construction
failure 在 replacement 期间只归还 candidate 容量；graph-load rollback 会归还两个尚未发布的
intent reservation。Live graph 或 failed-close graph 都不能提前释放其 grant。

## 边界与原理

两类当前 plugin ABI 都被明确标记为临时。Operation entrypoint name 会拒绝不受支持的 registration
generation，但已接受的 registrar table 与 callback 仍是 C++。Scheduler 数字 handshake 会在
discovery 或 object creation 前拒绝未知 interface generation，但通过握手后的边界仍以 C symbol
包装 C++ `ps::IScheduler*` 及其 vtable。人类可读 scheduler implementation version 只用于诊断，
不能替代数字 handshake。

两类接口的 binary compatibility 都依赖匹配的 Photospider SDK，以及兼容 compiler、标准库、C++ ABI、
allocator/runtime、exception model 与 RTTI configuration。C linkage 是 identity/generation gate，
不是纯 C data boundary 或跨工具链稳定性保证。

Shadow transaction 会阻止 registry 或 type map 局部发布；DSO lease 与匹配 destroy function 则让
callback state 和 plugin-created instance 保持在其定义库的生命周期内。这些机制会显式呈现临时
C++ 边界，而不会把 C symbol 错当成稳定 C value ABI。
[ADR 0003](../../adr/zh/0003-process-owned-execution-resources.zh.md)与精确的
[进程执行域目标](../../roadmap/zh/Kernel-Evolution.zh.md#进程执行域)记录已接受的 scheduler 替代
方向；精确的[服务器与插件隔离目标](../../roadmap/zh/Kernel-Evolution.zh.md#服务器与插件隔离)记录
operation isolation 方向。二者都不会改变本文说明的当前 loader contract。

## 兼容性指南

- 操作插件应使用已发布的注册 API 和公共数据契约。
- 操作插件必须使用 `ps::plugin::OperationPluginRegistrar` 和 `register_photospider_ops_v2`；v1 与无参数
  注册 ABI 都不受支持。
- 操作插件不得仅为了共享 registry 状态而链接 `photospider`。应链接
  `Photospider::operation_sdk`；只有使用 public OpenCV adapter 时才增加
  `Photospider::operation_opencv`。
- 调度器插件继承 `IScheduler`，实现其继承的 runtime contract，并导出精确数字 handshake 与其余六个
  必需函数。
- 调度器插件必须针对 ABI v2 重建，并把 Host 提供的 `[1,8]` `num_workers` 值当作该 instance 所拥有
  全部 worker thread 的 hard maximum。ABI v1 不受支持；仓库 example 还会防御性拒绝无效 direct call。
- Host 应使用插件 destroy 销毁插件创建的调度器实例。
- 当前不提供纯 C operation 或 scheduler ABI 兼容性。

## 实现与验证入口

- `include/photospider/plugin/plugin_api.hpp`
- `include/photospider/plugin/op_contract.hpp`
- `include/photospider/scheduler/scheduler.hpp`
- `include/photospider/scheduler/scheduler_plugin_api.hpp`
- `src/lib/plugin/operation_host_adapter.*`
- `src/lib/plugin/plugin_loader.*`
- `src/lib/plugin/plugin_manager.*`
- `src/lib/scheduler/scheduler_plugin_loader.*`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_plugin_manager.cpp`
- `tests/integration/test_scheduler_plugin_loader.cpp`
- `tests/unit/test_op_registry_m31.cpp`
- `tests/unit/test_scheduler_sdk_example.cpp`
- `tests/integration/graph_cli_plugin_compute_smoke.py`
