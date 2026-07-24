# 插件 ABI

Photospider 支持操作插件和策略插件。操作插件通过 Host 提供的 registrar 扩展进程拥有的
`OpRegistry`，它仍是临时 C++ ABI。策略插件通过带版本的纯 C ABI 对 Host 已准入的不可变
候选项排序；它不拥有工作线程、队列、设备、资源、Run 或 Graph 能力。可安装的插件开发契约
只位于 `include/photospider/plugin/` 和
`include/photospider/policy/policy_plugin_api.h`。

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
- `Photospider::operation_runtime` 只包含 value-factory 实现，不包含 registry、loader、Graph、policy、
  execution 或 compute state。
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
target，executor 或 reader snapshot 也可能保留同一个逻辑 target。这些路径可以并发调用它。因此，callback
provider 必须保证 target 可重入，或自行同步其共享可变 state。Registry 只串行化 ownership mutation、
coherent snapshot capture、publication 与 unload；它绝不会持有 state lock 来串行化 callback execution。
Caller 不得因为 operation key、device 或 intent 相同，就推断 callback 只会单线程执行。

仓库自有 CPU OpenCV provider 会用不可变 input、callback-local 或 task-owned `cv::Mat` state，
以及不使用进程范围的外层 operation mutex 来实现该契约。可选 builtin provider 会在 callback
发布前把 OpenCV 内部 CPU threading 固定为一，因此外层并行由 execution grant 拥有。其
provider-local fence 会把 OpenCV 资源耗尽转换为新建的 `std::bad_alloc`，并把其他
`cv::Exception` 转换为 host-owned `GraphError`。使用
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF` 构建会省略这些 slot，同时 registry 与 public
v2 registrar 仍可由其他 provider 使用。真实共享 backend state 仍要求 provider-local 同步：
Metal Perlin 的 DSO mutex 只保护其共享 Metal lifecycle。
[ADR 0004](../../adr/zh/0004-opencv-cpu-operations-are-reentrant-provider-work.zh.md)记录了该决策
及其 accounting 边界。

Direct replacement 同样遵循 manager-driven unload 之外的 retirement 规则。Replacement callback 会在
加锁前准备好，并与 active slot 交换；被替换的 callable 会留在参数局部 retirement value 中，直到
registry guard 已退出才析构。Whole-key unregister 会一起 extract legacy、metadata、implementation 与
ownership map node，再于 guard 外销毁移出的 value。因此 device implementation value 与其 revision
vector 始终保持平行；whole-key unregister 后的 direct device registration 不会继承 stale plugin token。
Manager-driven v2 registration 会把相同 slot 语义用于可选 OpenCV provider：DSO 可以拥有全部
active resize slot，通过 public `ImageBuffer` value 在不使用 OpenCV 的情况下执行，并在卸载时恢复
已捕获的 OpenCV predecessor。

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
## 策略插件 ABI

策略插件恰好导出由自包含 C11/C++17 头文件声明的两个函数：

```c
uint32_t ps_policy_plugin_get_abi_version(void);
ps_policy_status_v1 ps_policy_plugin_get_api_v1(
    ps_policy_plugin_api_v1 *out_api);
```

数字握手返回当前值为一的 `PS_POLICY_PLUGIN_ABI_VERSION`。只有精确相等后，
Host 才解析 `get_api_v1`。API 表包含四个必需回调：

| 回调 | 职责 |
| --- | --- |
| `get_metadata` | 返回 `[0,type_count)` 内某个索引对应的、可复制的类型条目。 |
| `create` | 创建一个特定类别的逻辑上下文。 |
| `select` | 从不可变原始快照选择一个候选项，或弃权。 |
| `destroy` | 恰好一次销毁一个成功创建的逻辑上下文。 |

所有状态、类别、掩码、决策 kind、结构 kind、标志、计数、大小和代次都使用
固定宽度整数域。受支持 ABI profile 要求八位字节、32 位 `uint32_t`、64 位
`uint64_t`、64 位指针，以及八字节的指针/整数对齐。编译期断言冻结每个记录
的自然布局：

| 记录 | 大小 | 对齐 |
| --- | ---: | ---: |
| `ps_policy_string_view_v1` | 16 | 8 |
| `ps_policy_type_metadata_v1` | 80 | 8 |
| `ps_policy_create_args_v1` | 40 | 8 |
| `ps_policy_candidate_v1` | 120 | 8 |
| `ps_policy_selection_snapshot_v1` | 64 | 8 |
| `ps_policy_decision_v1` | 48 | 8 |
| `ps_policy_plugin_api_v1` | 80 | 8 |

ABI v1 没有尾部扩展规则。Host 要求大小、结构 kind、字段偏移、回调指针、
枚举值、边界和零保留区全部精确匹配。打包 pragma 或不受支持的目标 profile
会触发头文件的布局断言；记录形状一旦变化，就必须引入新的 ABI 代次。

类型名是 1..128 个小写 ASCII 字节，并匹配 `[a-z][a-z0-9_.-]*`。
描述和实现版本会被复制，必须是最多 4,096 字节的有效 UTF-8。一份 DSO 暴露
1..256 个类型，并且不能使用 Host 保留名称 `interactive` 或 `throughput`。
支持类别掩码必须是 Interactive 和 Throughput 位的非零子集。

即使两个类别使用同一 DSO 类型，Host 也会为每个类别绑定创建独立逻辑上下文。
创建记录只包含类别和非零绑定代次。成功返回空上下文是合法的，但仍要求一次
destroy 调用。创建失败必须返回空指针，并回收插件的全部局部分配。

选择快照包含 1..4,096 条步长精确的候选项记录。候选项只包含不透明身份和
Host 生成的标量排序元数据。快照存储在 `select` 返回前是借用且不可变的；
插件不得保留它。选择结果回显精确的绑定与快照代次，并指定原始快照中唯一的
一个候选项。弃权必须返回零候选项 ID。两种结果都不会授予执行权限。

C++ 包含方式为导出和回调附加 C linkage 与 `noexcept`；C11 使用平台 C
调用约定。Host 仍对回调入口加保护，使错误的 C++ DSO 不能把异常对象导入
Host 状态。初始化阶段的 `OUT_OF_MEMORY` 变成新的 Host
`std::bad_alloc`；无效或不支持的初始化结果变成
`GraphErrc::InvalidParameter`；内部、未知或逸出的初始化故障变成
`GraphErrc::ComputeError`。选择故障则被归类为绑定代次局部的策略故障，
不会通过 Run 展开异常。

## 策略 SDK Target 与链接方式

策略插件请求 `policy_sdk` package component，并链接
`Photospider::policy_sdk`。这是一个只提供安装 include 目录和 C11/C++17
编译特性的 interface target。它不链接静态 `photospider` 产品、operation
runtime、OpenCV、注册表、执行器或任何拥有工作线程的实现。

策略 ABI 刻意不包含 C++ 标准库值、异常、RTTI 对象、虚接口、分配器所有者
或 Host 回调。兼容 DSO 可以在冻结的 64 位自然布局 profile 下使用 C11 或
C++17 编写。该 ABI 不承诺兼容不同的指针大小、对齐模型、调用约定或未来 ABI
代次。

`PHOTOSPIDER_POLICY_PLUGIN_EXPORT` 选择平台导出可见性，
`PS_POLICY_CALL` 选择声明的调用约定。插件只导出精确的两个名称。系统不存在
scheduler SDK target、`IScheduler` 基类、scheduler factory、工作线程数量
创建参数或兼容 shim。

## 策略插件加载事务

`PolicyRegistry` 是不可变内建类型记录和 DSO 类型记录的进程所有者。加载一份
DSO 时按以下顺序执行：

1. 拒绝空路径、含 NUL 的路径以及策略回调同线程发起的修改；
2. 归一化绝对路径，并以 eager/local 方式打开；
3. 只解析并调用 `ps_policy_plugin_get_abi_version`；
4. 要求 ABI 精确相等，然后解析并调用
   `ps_policy_plugin_get_api_v1`；
5. 校验完整且大小精确的 API 表；
6. 将每条元数据复制并校验到私有 map；
7. 在注册表锁内拒绝所有可见名称冲突，暂存完整的下一版类型/路径容器，并通过
   swap 同时发布两者。

缺少符号、ABI 不匹配、API 字节格式错误、无效 UTF-8、无效边界/掩码、保留的
内建名称、重复条目或可见名称冲突，都不会为该 DSO 发布任何类型或路径。检查
回调和借用元数据时，以及销毁暂存记录时，候选 DSO 租约始终存活。最终只有
完整复制到 Host 所有权的元数据可被观察。

注册表不会在持有 mutex 时调用 DSO 回调。版本、API、元数据、create、select
和 destroy 边界都标记为策略回调区间。回调可以重入只读注册表观察；同线程的
load、scan、unload、binding creation 或服务级策略修改，会在等待注册表锁或
绑定锁之前被拒绝。

`scan` 保持调用方给出的目录顺序，在每个目录内对匹配 DSO 候选项排序，并对
每个候选项调用同一个单 DSO 事务。它有意不把整个扫描做成一个事务：后续文件
系统操作或加载失败时，较早完整加载的 DSO 仍保持发布。

## 策略绑定与库生命周期

可见类型记录拥有复制的元数据、已校验 API 表、从零开始的条目索引以及共享 DSO
租约。绑定准备阶段在注册表锁内复制该记录，释放锁后调用 `create`，并在服务
发布前构造一个不可变的类别/代次/上下文所有者。内建策略使用同一套绑定、代次、
首故障和决策校验接口，但不调用 DSO。

Interactive 与 Throughput 绑定是不同上下文，各有独立非零代次。替换会在服务
发布锁外准备候选项。创建或发布失败时，活动绑定及其代次保持不变。成功发布会
退役旧的共享绑定；最后一个所有者会恰好一次、不重试地调用插件 `destroy`，
并在调用期间保持 DSO 映射。这个不抛异常的退役路径只把 destroy 状态和可捕获
故障作为诊断。成功的空上下文同样销毁一次。

每次选择在完整回调和校验区间内保留共享绑定所有权。Host 初始化完整的快照和
决策记录，在不持有注册表、绑定状态、就绪存储、资源账本、Graph 或 Run 锁时
调用回调，并根据不可变原始调用校验返回决策。第一次无效插件结果作为粘滞故障
存入对应的精确绑定代次。后续并发故障不能替换它。成功替换从无故障的新代次
开始。

注册表卸载会原子移除全部 DSO 条目和路径可见性，同时保留两个内建类型。已有
绑定继续保留其类型记录、回调表、上下文和 DSO 租约，因此在最后一次调用和最后
一个绑定所有者退役前一直有效。该卸载原语只用于测试和进程清理，不是公共 Host
生命周期命令。进程注册表本身有意具有进程生命周期。

诚实但永不返回的进程内回调可以无限期保留绑定和 DSO 租约。Host 不承诺跨该
边界提供超时、强制展开、销毁或卸载进度。进程隔离的插件监管属于单独的架构
代次。

## 边界与原理

当前两个插件边界刻意采用不同的兼容 profile：

| 边界 | 数据 ABI | 权限 |
| --- | --- | --- |
| 操作插件 v2 | 临时 C++ registrar 与回调值 | 在 Host 校验下执行操作计算并返回值 |
| 策略插件 v1 | 冻结 64 位 profile 下的精确大小纯 C 记录 | 只排序；不具备资源或执行能力 |

操作插件的 C linkage 入口名称只是身份/代次 gate，并不是稳定 C data ABI。
二进制兼容性仍依赖匹配的 SDK、编译器、标准库、C++ ABI、分配器/runtime、
异常模型和 RTTI 配置。

策略边界只使用固定宽度标量、不透明 `void *` 上下文、借用的不可变数组以及
C 函数指针。精确布局断言和校验明确规定受支持 profile，但不能沙箱化恶意 DSO。
插件仍在 Host 进程内执行受信任的原生代码，可能阻塞、破坏内存、在计账外分配
或创建未申报线程；它只是无法通过 ABI 合法获得执行能力。

影子发布阻止操作注册表或策略类型 map 局部可见。DSO 租约让回调状态和插件拥有
的值或上下文保持在其定义库的生命周期内。匹配的操作恢复 token 和策略绑定代次
可防止已移除或替换的插件静默夺回当前所有权。

[ADR 0003](../../adr/zh/0003-process-owned-execution-resources.zh.md)记录
进程级执行方向。[ADR 0007](../../adr/zh/0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
要求策略与执行保持分离，并禁止恢复旧的工作线程所有型 scheduler 边界。
[进程执行域目标](../../roadmap/zh/Kernel-Evolution.zh.md#进程执行域)和
[服务器与插件隔离目标](../../roadmap/zh/Kernel-Evolution.zh.md#服务器与插件隔离)
记录后续方向。

## 兼容性指南

- 操作插件使用 `ps::plugin::OperationPluginRegistrar`，并且只导出
  `register_photospider_ops_v2`；v1 和无参数注册 ABI 均不受支持。
- 操作插件链接 `Photospider::operation_sdk`；只有使用公共 OpenCV adapter
  时才增加 `Photospider::operation_opencv`。它们不得仅为了共享注册表状态
  而链接宽泛的静态产品。
- 策略插件包含 `photospider/policy/policy_plugin_api.h`、请求
  `policy_sdk` component，并链接 `Photospider::policy_sdk`。
- 策略插件导出精确的两个 v1 符号、填写大小精确的记录、保持每个 Host 初始化
  的前缀/保留字段，并且只返回已声明的状态/枚举值。
- 策略回调不保留快照内存，并把每个候选项 ID 当作不透明值。它们绝不创建工作
  线程，也不声称选择结果已经启动工作。
- 系统不存在 operation v1 兼容路径、scheduler SDK、scheduler ABI、
  `IScheduler` adapter 或执行路由插件 ABI。

## 实现与验证入口

- `include/photospider/plugin/plugin_api.hpp`
- `include/photospider/plugin/op_contract.hpp`
- `include/photospider/policy/policy_plugin_api.h`
- `src/lib/plugin/operation_host_adapter.*`
- `src/lib/plugin/plugin_loader.*`
- `src/lib/plugin/plugin_manager.*`
- `src/lib/policy/policy_registry.*`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_plugin_manager.cpp`
- `tests/unit/test_op_registry_m31.cpp`
- `tests/unit/test_policy_registry.cpp`
- `tests/integration/static_product_consumer_smoke.py`
- `tests/integration/graph_cli_plugin_compute_smoke.py`
