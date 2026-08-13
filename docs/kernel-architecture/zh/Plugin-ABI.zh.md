# 插件 ABI

Photospider 支持操作插件、数据定义 provider 和策略插件。操作插件通过 Host 提供的 registrar
扩展进程拥有的 `OpRegistry`，当前安装的边界仍是临时 C++ ABI v2。
[ADR 0012](../../adr/zh/0012-operation-plugins-use-a-separately-versioned-pure-c-abi.zh.md)
接受独立 pure-C operation-plugin ABI v1 作为替代目标；其 header、loader、SDK 与已迁移 plugin
尚不存在。数据定义 provider 通过纯 C ABI v3 发布不可变 Schema/Facet/Layout bundle 和受界限
约束的语义回调；它不获得 access、conversion、execution、device、registry mutation 或 graph
能力。策略插件通过纯 C ABI v1 对 Host 已准入的不可变候选项排序；它不拥有工作线程、队列、
设备、资源、Run 或 Graph 能力。当前可安装的开发契约只位于 `include/photospider/plugin/` 和
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

每个可执行 registration 都携带一个 `OperationMetadata` 值。除 tile、device、cost 与
dependency hint 外，CPU execution contract 还包含：

| 字段 | 含义 |
| --- | --- |
| `reentrant` | 精确 implementation 的 callback 是否可以重叠；默认 `true`。 |
| `maximum_parallelism` | 精确 implementation 的 callback 上限；零表示没有 implementation-specific cap。 |
| `retained_memory_bytes` | 每个飞行中 callback 额外占用的 Host-retained byte；零是显式声明。 |
| `scratch_bytes` | 每个飞行中 callback 额外占用的 Host scratch byte；零是显式声明。 |
| `exclusive_key` | 跨 implementation、Run 与 Graph 共享的可选 execution-domain exclusion key。 |

无论 `maximum_parallelism` 为何，`reentrant=false` 的有效上限都是一。非空
`exclusive_key` 最多 128 byte，且不能包含 embedded NUL。Host 会在发布前对 core 与 plugin
registration 执行相同校验。这些字段扩展了临时 C++ v2 metadata layout，但没有改变 registrar
symbol 或 callback signature。已有 v2 DSO 必须针对匹配 SDK 重新构建；不存在 missing-tail、
stale-layout 或 compatibility interpretation。

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
`Photospider::operation_runtime`；后者的 shared library 实现公共 image-buffer factory、
explicit-binding DenseTensor Value 与 checked view symbol、dependency-neutral device/access
fact，以及 Region value/algebra，不反向链接 SDK，也不要求外部 package。静态 Host product 与独立加载且使用
Value 的 operation DSO 因而都通过同一个 runtime image
解析 allocation/revision minting，而不会把 counter 复制进每个 DSO。这是普通 dynamic
dependency，不是 ELF/Mach-O symbol interposition 或 plugin ABI callback。这些 data/memory
header 可用于 dependency-neutral plugin-internal 工作，但 operation v2 callback
record 仍接收并返回当前 ImageBuffer/OperationOutput value。

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
- `Photospider::operation_runtime` 只包含 ImageBuffer、immutable DenseTensor value/view、
  Region value/algebra、provider-defined Value、extension envelope 和
  `DataDefinitionRegistry` 实现，不包含 operation/policy registry、loader、Graph、policy、
  execution 或 compute state。其唯一进程级 identity authority 持有彼此独立的单调 allocation
  与 Value-revision 序列；注入的 definition registry 则独立拥有自己的 provider generation
  序列。
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

## 本地插件信任准入

当前 operation-v2 与 policy-v1 DSO 的每次加载，都以一份共享、进程内不可变的 trust policy
开始。第一次授权时，进程读取 `PHOTOSPIDER_PLUGIN_TRUST_MANIFEST`、
`PHOTOSPIDER_PLUGIN_TRUST_SIGNATURE` 与
`PHOTOSPIDER_PLUGIN_TRUST_PUBLIC_KEY` 指定的三个文件，三者缺一不可。严格 LF 结尾的
canonical manifest 通过 OpenSSL EVP 使用 Ed25519 验证，并把每个已排序条目的封闭
`operation`、`policy` 或 `isolated-runtime` kind、非零 128-bit package id、正 generation 与
SHA-256 内容摘要绑定起来。重复 `(kind, package id, generation)` 身份与重复 `(kind, digest)`
内容角色映射都会被拒绝，因此字节与角色只会选择一个 package generation。首次成功策略或默认拒绝的配置结果会在进程生命周期内保留；之后的
environment 变化或 IPC value 都不能铸造或替换 trust authority。

在支持 exact-object 的 profile 上，授权会在不跟随最后 symlink 的条件下打开普通文件，从候选
hash 有界 byte 并检查前后 metadata 稳定，但绝不把该 mutable inode 作为权限返回。Linux 会把获批
byte 复制到 anonymous `memfd`，
应用 write/grow/shrink/seal 四种 seal，并在通过 `/proc/self/fd/N` mapping operation/policy 前于
sealed descriptor 上确认 SHA-256。因此在 Linux 上，rename、symlink、pathname replacement、
hard link 或 preopened writer 都不能替换后续 byte。Darwin 没有经过证明、能够抵抗同 UID 预开写
descriptor 的无特权不可变 exact-object primitive，因此会对 operation、policy 与 isolated-runtime
授权都在候选 path 访问前返回 `ExactObjectUnsupported`。当前 Windows 及其他所有不支持的 native
profile 使用同样的默认拒绝边界，不回退 pathname。

描述符 `N` 关闭后，`/proc/self/fd/N` 这一拼写不再是对象身份：第一份 DSO 仍保持映射时，
后续授权可能复用同一个编号。因而，每次 operation 或 policy 映射成功后，都会把它的精确
`AuthorizedPluginFile` capability 移入与 `dlopen` handle 相同的共享 native-library lease。
Callback、事务记录、policy type record 和活动 binding 都会保留这份组合 lease。最终释放会先
销毁插件拥有的 callback/context state，再调用 `dlclose`，最后才关闭 sealed snapshot
descriptor。Mapping、ABI、symbol、staging、publication、replacement 或 allocation 失败同样
保持“handle 先于 capability 退役”的顺序；Host 既不会在首次 mapping 后立即关闭 descriptor，
也不会把它永久保留在全局 cache 中。

每个 `PluginTrustError`（包括 trust 配置缺失/不可读、候选无法打开、未签名、kind 错误或
内容已变更）都会由 operation load report 或 policy Host surface 公开映射为
`GraphErrc::InvalidParameter`。成功 trust authorization 后发生的 native `dlopen` failure 仍映射
为 `GraphErrc::Io`；缺少 ABI symbol 或 ABI 内容畸形仍为 `GraphErrc::InvalidParameter`。这样会把
授权失败与“已经授权的本地对象无法映射”区分开来。

批准并不会 sandbox 进程内 DSO。获批 operation/policy library 仍是 operator-trusted native
code，照常可以阻塞、在 Host accounting 之外分配、发起 syscall 或破坏 Host process。下文独立的
isolated-runtime trust/resource control 不会给这两个进程内 ABI 追加 containment。

## 操作插件加载事务

加载单个 operation plugin 是覆盖 loader 全部可观察状态的强事务。在调用
`register_photospider_ops_v2` 或执行 native mapping 之前，loader 会先把精确已打开候选授权为
签名 `operation` artifact。随后它会为目标 `OpRegistry`、operation-source map、结构化 load
result 和 retained-handle map 创建 staged copy。Host 提供的 registrar 指向 staged registry，
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

可执行 scalar slot 是原子的可调度值，而不是 callback 加一份可变的 intent-wide
metadata record。Monolithic HP、tiled HP 与 tiled RT 各自在一个 `OpImplementation`
中拥有自己的精确 callback、`OperationMetadata` 与非零 implementation identity。
注册 sibling shape 不能改写已有 slot 的调度声明或 identity。Replacement、capture、
retirement 与 unload 会交换完整 slot，因此 reader 绝不会把一个 scalar callback 与另一次
registration 的 reentrancy、cap、retained byte、scratch byte 或 exclusive key 组合起来。

Live device implementation element 使用稳定、不可变的 owner，而不是把 `std::function` target 直接存进
会增长的 registry vector。新的 monolithic 或 tiled device value（包括其 plugin lease wrapper）会在获取
registry lock 前完整构造。加锁后，registration 只增长并发布 shared owner 及其平行 revision token。
Reader 在锁内保留一份一致的 owner 列表，只在释放 lock 后复制 callback target。第一个 CPU candidate 的
legacy HP compatibility slot 是持有同一 stable owner 的 forwarding bridge，不会复制原始 target。混合
plugin/direct 卸载期间，plugin-owned owner 会 swap 进预分配 retirement slot，后续 direct owner 则只会
swap 进已经清空的 gap；移除尾部时因此只会析构空 owner。任何已有或被移除的 device callback target
都不会在 registry lock 内被复制、移动、析构，也不会在锁内释放最后一个 library lease。新的稳定 value
或 compatibility bridge 构造失败发生在 key、callback 或 ownership 发布之前，并保持 registry 不变。

稳定所有权本身不等于执行 mutex。Registry 只串行化 ownership mutation、coherent snapshot capture、
publication 与 unload；callback execution 期间绝不会持有 registry state lock。Product planning
会选择一份 coherent callback、metadata、device 与非零 ownership revision，在 plan 中只保存
callback-free identity/metadata，并在 admission 前重新解析并要求精确 identity 相同。随后，在同一个
注入的 `ExecutionService` 内，Host 会在 reserved start 时跨 Run 与 Graph 执行 `reentrant`、
`maximum_parallelism` 与 `exclusive_key`。共享 operation key、device、intent 或 callback owner
本身不意味着串行，除非选中的 metadata 明确声明。Provider 仍必须保护从该 Host boundary 外部访问、
或者没有在声明中覆盖的共享 state。

仓库自有 CPU OpenCV provider 会用不可变 input、callback-local 或 task-owned `cv::Mat` state，
以及不使用进程范围的外层 operation mutex 来实现该契约。可选 builtin provider 会在 callback
发布前把 OpenCV 内部 CPU threading 固定为一，因此外层并行由 execution grant 拥有。其
provider-local fence 会把 OpenCV 资源耗尽转换为新建的 `std::bad_alloc`，并把其他
`cv::Exception` 转换为 host-owned `GraphError`。使用
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF` 构建会省略这些 slot，同时 registry 与 public
v2 registrar 仍可由其他 provider 使用。真实共享 backend state 仍要求 provider-local 同步：
但该要求只适用于 provider 真正拥有的 state。Metal Perlin DSO 既不拥有 native lifecycle，
也不拥有 lifecycle mutex；它会从当前进程 executor 借用 command queue、invocation-scoped
allocator 与 pipeline cache，而 execution metadata gate 则提供 implementation/key 串行化。
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
slot、把 native handle 与精确授权 capability 组合起来的 RAII lease，以及单调递增的成功加载序号。生产低层 loader
要求不可伪造的 process-owner token，因此 caller 无法用第二套 source/handle/restoration map 向全局
registry 发布 callback。`PluginManager` 是唯一生产加载入口；不存在接收 caller source map 或在加载事务
提交后复制 manager state 的 legacy wrapper。

每个 registrar callback 都由共享的组合 native-library lease 包装。因而，已解析 callback snapshot 在显式
全局 unload 移除 registry entry 后仍可安全调用，同时 mapping 及其 sealed exact-object descriptor 都保持
存活。Monolithic callback 的 public value 转成 host-private `NodeOutput` 后也会附着同一个 lease。该 lease 是第一个声明、最后销毁的 member；copy construction
会先保留 lease，再复制 payload
state；move construction 会通过 no-throw swap 转移完整 state。Copy/move assignment 会先暂存完整
replacement，再 swap 到位，并由 temporary 在释放旧 lease 前依次销毁旧
image/ParameterValue/spatial/debug state；copy 失败时 destination 保持不变。因此，即使在显式全局 unload 后
复制、移动或覆盖 cached output，plugin 定义的 image/context deleter 执行时仍保持 library mapped。
这些 lease 不反向引用 manager 或 registry，因此不会形成 ownership cycle。当最后一个
callback/value/generation owner 退役时，会先执行 native unload，再由匹配的 capability 关闭 sealed descriptor。

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

## 已接受的 Operation Plugin ABI v1 目标

Issue #101 冻结替代 contract，但不实现它。目标是独立 operation-plugin ABI v1，不是
provider-v3 suite 或 policy-v1 extension。其未来 self-contained C11/C++17 header 为
`photospider/plugin/operation_plugin_api.h`，discovery 只能使用：

```c
#if defined(__cplusplus)
extern "C" {
#endif

PS_OPERATION_PLUGIN_EXPORT uint32_t PS_OPERATION_CALL
ps_operation_plugin_get_abi_version(void) PS_OPERATION_NOEXCEPT;

PS_OPERATION_PLUGIN_EXPORT ps_operation_status_v1 PS_OPERATION_CALL
ps_operation_plugin_get_api_v1(
    ps_operation_plugin_api_v1 *api_out) PS_OPERATION_NOEXCEPT;

#if defined(__cplusplus)
}
#endif
```

Host 只在 numeric handshake 后请求 root API。`PS_OPERATION_PLUGIN_EXPORT` 是只用于
这两个具名 declaration 的平台 export/default-visibility annotation。
`PS_OPERATION_CALL` 是 platform C convention，Windows 为 `__cdecl`；
`PS_OPERATION_NOEXCEPT` 在 C++17 中为 `noexcept`，在 C11 中为空。两个
resolved entrypoint typedef 与每个 callback 都携带后两个 macro。V1 profile 冻结
8-bit byte、4/8-byte `uint32_t`/`uint64_t`、8-byte data 与每个具名
function-pointer type、natural 8-byte data/function-pointer 与 `uint64_t` alignment、
Host-process endianness 与匹配 convention。Packed、over-aligned、32-bit、foreign-endian
或 foreign-convention record 不兼容。Object pointer、function pointer 与 integer slot
保持为不同 C type。

只有 Diagnostic 至 Tile 这 20 个 versioned semantic record 以精确 `struct_size`、
`struct_kind`、`struct_version`、`flags` 开头。Plain fixed identity/handle、byte-view、
digest、array-reference、configuration-value、axis-range helper 不携带 record header；
root/suite table 使用各自 prefix。每个 suite 以 `struct_size`、`suite_id`、
`suite_version`、`flags` 这四个 `uint32_t` 字段开头。V1 拒绝 unknown kind
或 suite ID、version/flag、非零 reserved、short/long record、unknown tail、wrong stride/
alignment 与 arithmetic/range overflow。新增字段需要新 owning-suite version 或 operation
ABI v2；v1 不解释 minimum-size prefix。

Root API 精确为 96/8 byte/alignment，以 `struct_size`、`abi_version`、`flags`、
`reserved0` 开头，随后包含 permanent 128-bit plugin identity、bounded implementation-
version view、opaque plugin context、`query_suite`、
typed `destroy_plugin` 与精确的 `uint64_t reserved[3]`。`query_suite` 同样是 typed
field。每个 v1 suite table 精确为 64/8：

| ID | Suite | 要求 | Callback |
| ---: | --- | --- | --- |
| 1 | Definition | 始终 | operation count/get；implementation count/get |
| 2 | Configuration | 始终 | validate；create context；destroy context |
| 3 | Inference | 始终 | infer complete output plan |
| 4 | Region | 始终 | backward dirty；forward active-edge propagation |
| 5 | Dependency | 任一 implementation 声明 data dependence 时 | build dependency record |
| 6 | Execution | 始终 | synchronous monolithic；synchronous tiled |

每次 query 前，Host 把具体 64-byte suite 的每个 field 初始化为其 C semantic
zero value——integer field 为零、pointer field 为 null——并把其 nonnull
`ps_operation_suite_header_v1` 设置为 size 64、requested suite ID、requested version
与 zero flag。这不假定 byte-zero 就是 null pointer。Plugin 保留这四个 prefix
字段。Unknown ID 或 version 返回 `UNSUPPORTED`。
返回 `OK` 后，returned size/ID/version/flag mismatch 是 `INVALID_DESCRIPTOR`，并且
在读取 callback slot 前拒绝。缺失、malformed 或不完整的 required/declared
suite 会在 callback、source 或 handle 可见前拒绝整个 candidate。Table 不公开 allocator、
registry、Host service、Graph、Run、scheduler、cache、executor、device service、resource
token、filesystem、artifact、credential、thread 或 symbol lookup。

`get_api_v1` 前，Host 把具体 96-byte root 的每个 field 初始化为其 C semantic
zero value，并设置 size 96、ABI version 1、zero flag 与 zero `reserved0`；plugin
保留该 prefix。Host-prepared semantic record 与
suite 使用完整精确的 size/kind/version/flag 或 size/ID/version/flag prefix。
Plugin-authored sink record 携带 complete semantic-record prefix。Receiver 在读取任何
后续 field 前验证适用 prefix。

全部 suite/record version 都是 1。Suite ID 精确为 1 Definition、2 Configuration、
3 Inference、4 Region、5 Dependency、6 Execution。其他封闭 numeric set 包括：
ADR catalogue 中 Diagnostic 到 Tile 的 record kind 1 至 20、configuration kind 1 Null 至
8 Object、direction 1 Input/
2 Output、intent bit 1 HP/2 RT、shape bit 1 Monolithic/2 Tiled、device 1 CPU、access bit
1 Read/2 Write、behavior bit 1 SideEffect/2 DataDependent、Region outcome 1 Exact 至
4 Unknown、Region atom 1 Whole 至 4 TensorSlice、sink channel 1 Diagnostic 至
4 DependencyRecord。Unknown value/bit 与 invalid zero 以 `INVALID_DESCRIPTOR` 失败；
boolean 为 0 或 1。ValueView flag bit 1 是 PayloadAvailable；其他 semantic-record flag
与所有 root/suite flag 在 v1 中均为零。

ADR 0012 冻结了完整 normative C typedef prototype，而不只是 callback order。
Resolved entrypoint 是 `ps_operation_plugin_get_abi_version_fn_v1` 与
`ps_operation_plugin_get_api_fn_v1`；root 使用
`ps_operation_query_suite_fn_v1` 与 `ps_operation_destroy_plugin_fn_v1`；sink 使用
`ps_operation_emit_fn_v1`；Definition、Configuration、Inference、Region、Dependency 与
Execution 按 table 顺序精确使用 4/3/1/2/1/2 个具名 typed callback。除 numeric
entrypoint（`uint32_t`）与仅可为 null 的 reserved callback（`void`）外，每个 callback
都返回 `ps_operation_status_v1`。Sink prototype 精确包含 `void *host_context`、
`uint32_t channel`、`const void *records`、`uint32_t count`、`uint32_t stride`。
所有 identity/view/record/helper input 与 sink 都是 `const` pointer，Host output 是 writable
pointer，index/count/channel/stride 是 `uint32_t`，opaque context 是 `void *`，array
input 是 `const ps_operation_array_ref_v1 *`。即使 context 为 null，Configuration destroy
仍接收 operation/implementation identity。不公开 cancellation callback；
Host 在 entry 前、sink call 时、return 后检查，可以 normalize 为 `CANCELLED` 并丢弃 late
result。

Plugin、operation、implementation、port、Schema、Facet、Layout identity 是 permanent
publisher-assigned definition identity。Value、edge、allocation、binding、site、Region
identity 是 Host-minted process-local runtime identity，分别限定于 logical value、Graph
revision、allocation、binding/write grant 或 invocation snapshot。Host 另行以不同 helper
type 生成 nonzero、unpredictable 128-bit generation/invocation handle。这些 handle 只是
process-local correlation handle，不是 semantic identity、pointer、lookup API、capability、
durable identity、resource token 或 wire value。Invocation-scoped callback 携带两者；
definition/configuration-lifetime/root/destroy callback 依赖精确 DSO generation lease 与显式
identity/context。Plugin/configured-operation context 是
plugin-owned opaque `void *` round-trip value。成功 null context 有效，仍得到精确一次 matching
destroy attempt；create 失败不转移 destroy obligation。

Sink `host_context` 是 Host-owned callback-local round-trip token。Plugin code 只能把它传回
`emit`，不得 dereference、free、retain 或解释为 semantic identity。

未来 header 冻结以下 natural record size/alignment class；详细 ordered field group 在
ADR 0012 与 active OpenSpec design 中具有规范性：

| Layout category | Size/alignment |
| --- | --- |
| record header / suite header | 16/4 与 16/4 |
| identity、generation/invocation handle、immutable/mutable byte view、exact-stride array reference、configuration value、axis range | 16/8 |
| SHA-256 digest | 32/8 |
| diagnostic、output sink、configuration view、Region-set view | 48/8 |
| configuration node、facet view、tile | 64/8 |
| buffer view、input binding、Region binding | 80/8 |
| output plan、mutable output binding、invocation、Region atom、dependency record | 96/8 |
| port descriptor | 112/8 |
| operation descriptor 与 value view | 128/8 |
| value descriptor | 192/8 |
| implementation descriptor | 192/8 |
| root API 与每个 suite v1 table | 96/8 与 64/8 |

Active OpenSpec design 冻结每个 C field type 与 byte offset、每个精确 type/typedef
spelling，以及它显式给出的每个 field name。
29 个 fixed-layout payload type 精确由九个 plain helper 与 20 个 semantic record 组成；
record/suite header、root 与 suite table 是单独的 prefix/table type。128-byte operation
descriptor 能够成立，是因为 offset 96/112 分别为两个 16-byte input/output port
`{pointer,count,stride}` helper，不重复存储 count。Root 的 exact prefix 位于 0，plugin
identity 位于 16，version view 位于 32，context/query/destroy 位于 48/56/64，三个
`uint64_t` reserved word 从 72 开始。六个 suite table 从 offset 16 使用冻结的具名
typed callback slot，并以为 null 的 `ps_operation_reserved_callback_fn_v1` slot 补足
byte 63；不使用 object pointer 或 integer slot 代替 function pointer。

全部 pointer/count/stride view 只在一次同步 call 或 sink emission 中借用。Null 精确对应
zero count，stride 等于 exact element size；任一侧都在 dereference 前检查 alignment、
multiplication、base/offset、subrange、aggregate bound、output overlap。Host 在 return 前
deep-copy accepted descriptor/emitted result。

Operation/port descriptor 冻结 permanent identity、canonical name、borrowed exact-stride
port array、direction、configuration schema 与 Schema/Facet/Layout identity。Type/subtype
是不含 NUL 或 `:` 的 nonempty UTF-8，并组成唯一 `type:subtype` key。Port/implementation
name 是不含 NUL 的 nonempty UTF-8；display/exclusive key 可为空，非空时是不含 NUL 的
UTF-8。Name 不 normalize、case-fold 或截断。Implementation descriptor 冻结 HP/RT intent、
monolithic/tiled shape、CPU device profile、tile/access/side-effect/data-dependence fact、
reentrancy、maximum parallelism、retained/scratch byte、relative cost 与 exclusive key。
Callback、copied metadata、implementation identity、source generation 与 revision 作为一个
slot 一起发布和恢复。

Configuration 是 Host-owned immutable exact-stride tree，节点为 null、boolean、signed
64-bit integer、binary64、UTF-8 string、bytes、array、object，不是 YAML、`ParameterMap`
或 C++ variant。Value record 区分 Schema/Facet/Layout identity/version、logical revision、
allocation/binding identity 与 descriptor/content/layout digest。Inference、Region、
dependency call 只接收 payload pointer 已清空的 descriptor-only view。只有 execution
接收 payload，只有 Host mutable-output grant 允许写入。

Inference 在 allocation 前生成每个 immutable output plan。Region v1 明确提供 backward
dirty 与 forward active-edge propagation，outcome 为 Exact、Whole、Empty、Unknown，atom
为 Whole、Empty、ImageRect、TensorSlice。Data-dependent implementation 必须提供
Dependency v1；copied record 在 cache 前把 output port/site/region fact 绑定到 input
edge/region fact。

Execution v1 同步且只支持 CPU-addressable buffer。它不公开 native device handle、device-
resident buffer、fence、deferred completion、retained invocation owner 或 delayed sink。
Plugin 只有在 return 前复制到 Host CPU output 时才能私下使用 device。仓库 Metal 示例
必须使用 synchronous CPU staging，或在 v2 删除前移到 Host-private adapter 后面。Native/
async 工作需要未来独立 suite 或 ABI。

V1 没有 allocator callback。Definition string 在 return 前复制，execution 写 Host-owned
buffer，planning/diagnostic record 使用一个 callback-local 48-byte Host output sink。其
closed channel 按需接受 diagnostic、output-plan、Region-binding 或 dependency record。即使
plugin 忽略并返回 success，第一次 sink failure 仍 sticky。Host memory 由 Host destroy；
plugin memory 与成功 context 在精确 DSO lease 下得到一次 plugin destroy attempt。

`ps_operation_status_v1` 的精确 `uint32_t` 值 0 至 8 分别为 `OK`、
`INVALID_ARGUMENT`、`OUT_OF_MEMORY`、`UNSUPPORTED`、`INVALID_DESCRIPTOR`、
`TOO_COMPLEX`、`CANCELLED`、`FAILED_PRECONDITION`、`INTERNAL_ERROR`。Unknown status
作为 ABI fault 失败。一个 non-OK callback 可 emit 一条最多 4 KiB、被复制的 UTF-8
diagnostic。Exception/foreign unwind 不跨 DSO；C++ wrapper 在 plugin 内映射
`std::bad_alloc`、invalid input 与其他可捕获 failure。

实现必须在移除 C++ boundary 的同时保留当前 strong shadow transaction。它验证并复制
完整 root、suite、descriptor、callback、identity、bound，分配一个 Host generation，再原子
发布 immutable per-slot callback/metadata/identity/source/revision value。每个 callback/context
保留精确 generation 与 DSO。Retirement 先移除 publication，等待 lease，reverse destroy，
最后 unmap。Middle-generation unload 只 splice owned predecessor。Plugin code 执行期间不
持有 Host registry、publication、scheduler 或 execution lock。

进程内 callback 永不返回时，可能永久保留 invocation、write grant、context、generation 与
DSO。因此 operation ABI v1 只是 operator-trusted compatibility/validation boundary，绝非
sandbox。Tenant-untrusted pointer-free wire record、runtime supervision、trust/resource
enforcement 是分别版本化的独立边界，由 Issues #102、#103、#104 交付。Issue #104 现在保护
当前 operation-v2/policy-v1 native admission 与私有 isolated runtime 组合；它不实现本目标 ABI、
不选择最终用户 operation，也不增加通用 syscall/network sandbox。

后续 migration 先新增 v1，并迁移每个仓库 operation 与 installed consumer，然后在同一
release 删除 v2。不保留 wrapper、alias、dual loader、forwarding header、v2-to-v1 adapter、
missing-tail interpretation 或 runtime fallback。在这些实现与测试门禁通过前，本节只是目标，
前述 v2 章节仍描述当前事实。

## 数据定义 Provider ABI v3

数据定义 provider 精确导出由自包含 C11/C++17 头文件声明的两个函数：

```c
uint32_t ps_data_provider_get_abi_version(void);
ps_data_status_v3 ps_data_provider_get_api_v3(
    ps_data_provider_api_v3 *api);
```

数值握手必须返回 `PS_DATA_PROVIDER_ABI_VERSION`，其精确值为三。在等值检查成功前，
Host 不会调用候选项的其他任何函数。随后，Host 提供一张预先清零且大小精确的 API 表。
Provider 返回一个非零永久 provider identity、一个受界限约束的 implementation version、
一个受界限约束且不可变的 typed definition array、一个 opaque context，以及全部必需回调：

| 回调 | V-14 职责 |
| --- | --- |
| `validate` | 校验完整 Schema/Facet/Layout 以及经过检查的多 buffer payload。 |
| `query` | 计算一个纯 metadata-only property 请求。 |
| `evaluate_region` | 计算一个纯且受界限约束的 metadata-only Region 请求。 |
| `evaluate_spec` | 计算一个纯且受界限约束的 metadata-only DataSpec 关系。 |
| `visit_content` | 按 provider 的 canonical 顺序追加逻辑内容 byte。 |
| `create_owner` / `destroy_owner` | 在精确 generation 保持映射期间拥有一个可选 opaque object。 |
| `destroy_provider` | 在 module lease 释放前完成最终 generation 退役。 |

这只是 definition suite。V-14 不包含 access/map/import/transfer、conversion、inference、
execution、asynchronous completion、native-device 或 operation ABI replacement 回调。
纯 property、Region 与 DataSpec 调用会接收 descriptor、Layout、envelope、byte size、index、
role 和 allocation identity metadata，但所有 payload pointer 与 payload-available flag 都会被清除。
只有显式 validation 和 canonical-content traversal 能在保留调用期间访问 payload。

每个 callback 还会接收一个 callback-local `ps_data_output_sink_v3`。借用的
`ps_data_bytes_v3` view 只用于输入：diagnostic 与 BYTES property 携带 scalar
`message_size` / `bytes_size` 字段，并在 provider 源数据仍存活时，通过 output sink 复制完整
变长字段。非空 diagnostic 恰好使用一次 diagnostic channel；空 diagnostic 不使用该 channel。
Available BYTES property 即使为空也恰好使用一次 property channel。Host 会在解引用前检查
channel permission、pointer/count framing、重复使用、4 KiB diagnostic 上限与 64 KiB property
上限，同步复制到每次 invocation 私有的 state；即使 provider 忽略 sink error，第一个 failure
仍具有权威性。因此 callback return、并发调用、generation replacement 与 module retirement
都不会暴露延迟读取的 provider output pointer。

`visit_content` 使用的 `ps_data_byte_sink_v3` 是独立的同步 streaming channel，而不是上述
受限的 diagnostic/property field。Host 可以为一个 digest 多次调用 callback：它先用 checked
`uint64_t` 累计计量，写入冻结的 canonical field length，再在同一个不可变 Value view 与
payload read lease 下重复调用同一 active generation，同时直接 hash 每个 segment。每次
invocation 拥有独立的 diagnostic 与 sticky-failure state。Provider 必须重现相同逻辑 byte
sequence，但 append-call 边界可以不同。Host 会拒绝 null/nonzero pointer-count 对、计量
overflow、被忽略的 sink failure 与 measured/hash count 漂移。累计 stream 不会被物化，也不受
4 KiB/64 KiB output bound 或任意 64 MiB content 上限约束；它只受冻结的 SHA-256 length
framing 限制。

ABI adapter 的 Region request 仍是 rank-general。Provider 对规范非空 TensorSlice 返回 Exact
时，Host 会计算全部半开轴长度的 checked `uint64_t` product。Provider 的
`selected_site_count` 必须精确匹配；product overflow、错误的非零 count，或非空 slice 的零
count，都会返回 count 为零的 InvalidDescriptor。Empty 与非 Exact outcome 保持既有带类型语义。

所有记录都使用固定宽度标量、借用且受界限约束的输入 view、精确 struct size、必须为零的
reserved storage，以及平台 C calling convention。支持的 profile 要求 8-bit byte、8-byte
data/function pointer 和自然 8-byte alignment。头文件冻结以下 v3 布局：

| 记录 | 大小 | 对齐 |
| --- | ---: | ---: |
| `ps_data_identity_v3` / `ps_data_bytes_v3` | 16 | 8 |
| `ps_data_definition_v3` / `ps_data_extension_v3` | 64 | 8 |
| `ps_data_buffer_view_v3` | 56 | 8 |
| `ps_data_buffer_envelope_v3` | 48 | 8 |
| `ps_data_value_view_v3` | 88 | 8 |
| `ps_data_diagnostic_v3` | 48 | 8 |
| `ps_data_property_query_v3` / `ps_data_property_result_v3` | 40 / 56 | 8 |
| `ps_data_region_request_v3` / `ps_data_region_result_v3` | 72 / 40 | 8 |
| `ps_data_spec_request_v3` / `ps_data_spec_result_v3` | 64 / 40 | 8 |
| `ps_data_byte_sink_v3` | 40 | 8 |
| `ps_data_output_sink_v3` | 40 | 8 |
| `ps_data_provider_api_v3` | 160 | 8 |

这里没有 tail-extension 规则。Host 会拒绝非预期的 size、offset、kind、enum、bound、
pointer/count pair、必需 callback、reserved byte 或重复 typed key。Definition name 是诊断用途的
小写 ASCII `[a-z][a-z0-9_.-]*`；永久 128-bit identity 和非零 structural version 才具有权威性。
Schema、Facet 和 Layout 位于三个独立 typed namespace，因此不同 kind 使用相同数值 identity
不会冲突。

`DataDefinitionRegistry` 是注入的 C++ 权威，不是 global 或 function-static singleton。
它拥有一个 publication mutex、一个 generation source、一个 provider map 和三个 typed
definition map。候选项加载会复制并校验完整 bundle，暂存全部 next map，然后将它们一起发布。
ABI 不匹配、回调失败、metadata 畸形、重复项或由另一 active provider 拥有的 typed key 都不会
发布任何内容，并保留旧 generation。Provider callback 绝不在持有 registry mutex 时执行；
provider callback 对 registry 的同线程 mutation 会被拒绝。

Replacement 按永久 provider identity 执行，并原子发布一个全新完整 generation。Unload 会从新查找
可见性中移除 active generation。已有 `DataDefinitionLease`、provider-defined `Value`、带 index 的
`ProviderReadLease`、callback staging 和 `ProviderOwner` 值会让正在退役的 callback、context、
definition 与 module lease 保持存活。最终 owner destroy 恰好运行一次；最终 provider destroy
在全部 generation user 之后、module lease 释放之前运行。

如果最后一个 `ProviderOwner` 或 generation 引用在同一 Host 线程的任意 provider callback 内释放，
Host 不会递归进入对应的 destroy callback。Owner/generation state 内嵌自己的 cleanup node，因此最终
shared release 无需分配即可将它追加到 per-thread FIFO。外层 callback guard 会先清除 active-callback
fence，再在 provider code 返回后 drain 该 FIFO；provider 返回成功、返回失败以及 Host invocation
进行正常 C++ stack unwinding 时都遵循此规则。Destroy callback 或 cleanup member 析构所释放的 owner
或 generation 会加入 FIFO 尾部；单次迭代式 drain 会保留已有 FIFO 顺序，并防止 cleanup callback
递归进入。Provider callback 外的释放使用同一队列，但会同步 drain。Owner state 在整个
`destroy_owner` 期间保留其精确 generation，generation state 在整个 `destroy_provider` 期间保留
module lease，callback-tail 级联也不例外。正常 callback 返回会在正常 thread exit 前清空 per-thread
queue；永不返回的 callback 仍可无限期保留其 generation。这项 Host-side lifetime 修复不会改变
任何 v3 record layout、callback signature 或 provider responsibility。V-14 不提供强制展开或
进程隔离。

## 数据定义 SDK Target 与链接方式

Producer 请求 `data_provider_sdk` package component，并链接
`Photospider::data_provider_sdk`。这个纯 interface target 携带安装后的 include 目录以及
C11/C++17 compile feature。它不链接 operation runtime、静态产品、OpenCV、yaml-cpp、Threads、
registry、loader、executor 或 device SDK。C11 和 C++17 producer 导出相同的两个精确 C 名称；
C++ 声明使用 `extern "C"` 和 `noexcept`。

实例化 `DataDefinitionRegistry` 或创建 provider-defined `Value` 的 C++ Host-side consumer，
直接链接 `Photospider::operation_runtime`，或通过 `Photospider::operation_sdk` 间接链接。
提供平台解析出的 function pointer 和非空 module lease 是显式的 composition 职责；V-14
不会安装目录 scanner 或第二个 mutable registry authority。Dependency-disabled install smoke
会从安装包独立构建并运行采用精确名称的 C11 与 C++17 producer，编译独立的 output
record/sink layout assertion，从 callback-local storage 发出非空 property，并通过真实 registry
transaction 分别加载它们。

V-15 新增一个必须单独请求的 installed component：`openexr_deep_provider`。当
`PHOTOSPIDER_BUILD_OPENEXR_DEEP_PROVIDER=ON` 时，请求该 component 会导入
`Photospider::openexr_deep_provider`，随后发现 `OpenEXR::OpenEXR`；只请求 neutral
component 时，两件事都不会发生。使用默认 OFF build 时，optional request 会报告该 component
不可用，required request 则会在 OpenEXR discovery 前，以 Photospider 自有 component diagnostic
失败。Installed target 只有 provider MODULE；其 source-private C++ codec adapter 既不安装也不
export，而 provider 仍只暴露冻结的两个 v3 C entry point。

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
2. 归一化绝对路径，把精确已打开对象授权为签名 `policy` artifact，并保留该 capability；
3. 通过已授权 descriptor path 以 eager/local 方式打开，然后把 capability 移入由此得到的
   共享 native-library lease；
4. 只解析并调用 `ps_policy_plugin_get_abi_version`；
5. 要求 ABI 精确相等，然后解析并调用
   `ps_policy_plugin_get_api_v1`；
6. 校验完整且大小精确的 API 表；
7. 将每条元数据复制并校验到私有 map；
8. 在注册表锁内拒绝所有可见名称冲突，暂存完整的下一版类型/路径容器，并通过
   swap 同时发布两者。

缺少符号、ABI 不匹配、API 字节格式错误、无效 UTF-8、无效边界/掩码、保留的
内建名称、重复条目或可见名称冲突，都不会为该 DSO 发布任何类型或路径。检查
回调和借用元数据时，以及销毁暂存记录时，候选的组合 handle 与精确 capability 租约始终存活。
最终只有完整复制到 Host 所有权的元数据可被观察。Native open 后发生任何拒绝时，都会先关闭
handle，再释放 capability。

注册表不会在持有 mutex 时调用 DSO 回调。版本、API、元数据、create、select
和 destroy 边界都标记为策略回调区间。回调可以重入只读注册表观察；同线程的
load、scan、unload、binding creation 或服务级策略修改，会在等待注册表锁或
绑定锁之前被拒绝。

`scan` 保持调用方给出的目录顺序，在每个目录内对匹配 DSO 候选项排序，并对
每个候选项调用同一个单 DSO 事务。它有意不把整个扫描做成一个事务：后续文件
系统操作或加载失败时，较早完整加载的 DSO 仍保持发布。

## 策略绑定与库生命周期

可见类型记录拥有复制的元数据、已校验 API 表、从零开始的条目索引，以及把 native handle 与
精确授权 capability 组合起来的共享 DSO 租约。绑定准备阶段在注册表锁内复制该记录，释放锁后调用 `create`，并在服务
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
最终 lease 释放会先 unmap DSO，再关闭精确 sealed descriptor。

诚实但永不返回的进程内回调可以无限期保留绑定和 DSO 租约。Host 不承诺跨该
边界提供超时、强制展开、销毁或卸载进度。进程隔离的插件监管属于单独的架构
代次。

## 边界与原理

当前三个扩展边界与已接受替代目标刻意采用不同的兼容与权限 profile：

| 边界 | 数据 ABI | 权限 |
| --- | --- | --- |
| 操作插件 v2 | 临时 C++ registrar 与回调值 | 在 Host 校验下执行操作计算并返回值 |
| 操作插件 v1 目标 | exact-size pure C、独立版本化 suite、Host sink、同步 CPU grant | 未来在 Host 校验下执行 operation definition/planning/execution；尚未安装 |
| 数据定义 provider v3 | 冻结 64 位 profile 下、大小精确的纯 C definition-suite 记录 | 只执行 Schema/Facet/Layout 校验和受界限约束的语义观察 |
| 策略插件 v1 | 冻结 64 位 profile 下的精确大小纯 C 记录 | 只排序；不具备资源或执行能力 |

### 已实现的 V-2 至 V-15 SDK 与 definition-provider 子集

[ADR 0008](../../adr/zh/0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md)
接受分别版本化的纯 C provider suite，用于 Schema、Facet、Layout、access、conversion、
inference、query、region、digest 与 execution。V-14 实现上文所述的精确 v3 definition suite，
并同时提供公共 C++ envelope、`DataDefinitionRegistry`、generation lease、provider owner 和
provider-defined `Value` access。它不会把 STL、exception、RTTI、virtual class、allocator
ownership 或 `Value` PImpl 放到 C 边界上。Access、conversion、inference、execution、
asynchronous 与 native-device suite 仍是未来各自受界限约束的工作，不属于隐含的 v3 权限。

V-2 在 `operation_runtime` 中实现 dependency-neutral C++ CPU DenseTensor `Value`、
`StridedLayout`、`DenseTensorView` 与 `ImageView` 子集。V-3 新增 installed BufferHandle
range、read/write lease、ValueBuilder seal、byte offset、受界限约束的 signed immutable view
与 process-local allocation/revision identity。该 runtime 为 shared，因此 Host 与每个使用
Value 的 DSO 都调用同一个 minting authority；长期 loader regression 会打开两个独立 DSO，
并证明两类 identity 均保持不同。一条内建 operation 会在 private 双重表示 bridge
后使用该 surface：它保留 sealed result Value，再派生 ImageBuffer compatibility snapshot。

V-4 把 installed `RegionSet` 与 bounded algebra 加入 `operation_runtime`。Region 不会进入新的
public v2 slot。Source-private bridge 只识别通过 execution 的 route-visible device inventory
与 request intent 选出的实际 revisioned implementation 中的精确 core dense callback，并调用其
Region-aware implementation。该 bridge 绝不会执行 scalar-only lookup，也不会通过过滤 candidate
强制回退到 core；route 选中的 same-key device 或 plugin override 保持普通 complete-output v2
behavior。精确 ImageRect 可以适配当前 propagation callback，而 TensorSlice 绝不跨越
rectangular v2 contract。

V-6 把 installed、dependency-neutral 的 `ReadyFence` observer 与同步 Value readiness 加入
`operation_runtime`。Pending publication authority 与 `ValueTransferTask` 保持 source-private，
因此 SDK consumer 可以观察 readiness，但不能创建 pending producer 或 transfer task。

V-8 新增 installed、经过检查的 `DeviceBackend`、`DeviceId`、`MemoryDomain`、
`StorageBinding`、producer identity 与纯 `AccessPlan` observation。Native allocation
construction、native handle、mutable pending-device producer、completion admission、
`ResidencyManager` 与 CPU/Metal transfer submission 都保持 source-private。仓库 Metal operation
只通过 source-private invocation 边界接收借用的 device/queue/allocator context，在内部发布
pending native Value，并执行显式 asynchronous readback。第三方 v2 callback 不会获得这个
context，也不能从 `ImageBuffer::context` 推断它。

V-14 新增 installed、保留 byte 的 Schema/Facet/Layout envelope、typed
property/DataSpec/Region outcome、带 tag 的 canonical digest、artifact-envelope serialization、
精确的 v3 C 头文件，以及一个可注入的 C++ registry。Runtime 使用 dependency-neutral 的合成
`VariableSampleField` generation，证明 provider-defined multi-buffer Value。通用 Host validation
先于 provider callback 和 identity mint；纯调用不暴露 payload；content traversal 控制逻辑 byte，
但绝不拥有 Host digest state；旧 Value、read、callback 与 owner 在原子 replacement 和 unload 后
继续存活。这个切片既不安装平台 DSO scanner，也不让 provider-defined Value 进入 graph compute、
operation ABI v2、cache policy 或 codec。

V-15 提供同一套未扩展 definition suite 的一个仓库自有实现。该 module 精确发布四项
definition：`VariableSampleField` Schema、`ImageFacet`、`DeepSampleFacet` 与 deep
multi-buffer Layout，并通过既有 callback 校验其版本化 payload 与完整 Value envelope。其显式
implementation-version byte 只用于诊断；永久 provider/definition identity 与 structural version
决定 interpretation。该 module 不 export codec entry point，也不拥有 registry、path policy、
executor、cache 或 commit policy。Source-private adapter 会执行 OpenEXR read/write，并在保留
module lease 时调用普通 registry/Value API。因此 OpenEXR 是一项可选 codec/provider 实现，
而不是新的 v3 权限或第四条 ABI 边界。

这些切片都不会把 Value、BufferHandle、lease、Region、ReadyFence、device/access record 或
PImpl 放进 v2 callback record。V-14 在不改变另外两个边界的前提下向表中加入第三个边界。
在每个仓库自有 operation 与 installed consumer 完成 migration 前，
operation ABI v2 仍是当前 operation contract。完成边界随后会删除 v2、其 entry point、SDK、
fixture 与 package surface，不保留永久 dual loader、wrapper、alias、forwarding header 或
v2-to-v1 shim。替代项是 ADR 0012 接受的独立版本化 operation-plugin ABI v1，不是
provider-v3 suite。Policy ABI v1 继续独立版本化。

操作插件的 C linkage 入口名称只是身份/代次 gate，并不是稳定 C data ABI。
二进制兼容性仍依赖匹配的 SDK、编译器、标准库、C++ ABI、分配器/runtime、
异常模型和 RTTI 配置。

策略边界只使用固定宽度标量、不透明 `void *` 上下文、借用的不可变数组以及
C 函数指针。精确布局断言和校验明确规定受支持 profile，但不能沙箱化恶意 DSO。
插件仍在 Host 进程内执行受信任的原生代码，可能阻塞、破坏内存、在计账外分配
或创建未申报线程。签名 immutable-snapshot admission 会限制哪些 byte 及 role 可以进入进程，但不会
改变这些能力；插件只是无法通过 ABI 合法获得执行能力。

Issue #93 不会改变上述三条 ABI 的 inventory 或 record layout。其 `I1Host`、
`ComputeRunObservationSink`、accepted-boundary collector、inner-row evaluator 与精确 runner
均是 source-private benchmark/Host mechanism。它们不会进入 installed Host request、operation
registrar record、data-provider v3 record、policy-plugin v1 record、SDK target、IPC 或 CLI。
Observer 只接收 copied fact 与 immutable final Value；它不是第四条 extension boundary，也不向
DSO 暴露 callback。

Issue #94 同样不会改变 installed ABI inventory、layout、symbol 或 package component。
`ProgressiveComputeOptions`、`ProgressiveFinalGate`、I2 Host/profile/evidence type 及其
observation callback 都保留在 `src/lib` 下；installed Host request、IPC 或 CLI grammar、
operation registrar、data-provider v3 table 与 policy-plugin v1 table 都不会新增 field。精确
preview primitive 是内部 CPU helper。Rank-three HWC Metal upload 是既有进程自有 executor 的
内部泛化，并通过未改变的 `Value`、`AccessPlan`、residency 与 ledger contract 发布；它既不
export native handle，也不新增 provider callback。手工 runner 与 deterministic test 只是这些
私有 seam 的 consumer，不是 SDK 或 extension surface。

已安装的 `compute_content_digest(Value)` 现在除既有 provider-defined traversal 外，也会通过
Host/runtime canonical-v1 实现处理内建 DenseTensor value。内建路径使用保留的
Schema/ImageFacet identity、descriptor metadata 与 logical payload byte；它不会调用或新增
data-provider callback。Provider-defined value 继续使用未改变的 v3 mandatory `visit_content`
callback。因此 data-provider API table 仍为 160 byte，冻结的 v2/v3/v1 record 与 symbol
inventory 均保持不变，也不会引入新的 compatibility generation。

执行画像证据不会加强这条信任边界。有效的 `execution-profile-slo-v1` 行会冻结并
hash 精确的进程内 operation/provider 与 policy generation，拒绝未申报的 worker
pool 或 resource authority，并把当前 ledger/device authority 之外的 allocation
作为 diagnostic，而不是静默算入 memory。其 latency、throughput、fairness、
determinism、waste 与 memory 判定不声明 sandbox 或 hostile-code containment。
[ADR 0010](../../adr/zh/0010-execution-profile-slos-are-six-independent-benchmark-verdicts.zh.md)
定义这项证据边界；process supervision 与 isolated invocation 仍属于独立的
server/plugin-isolation 目标。

[ADR 0011](../../adr/zh/0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.zh.md)
冻结了该目标边界。Server control plane 与 WorkerManager 不加载任何 DSO。凡是加载到 Host
进程内的 DSO 都仍是 operator-trusted native code，其中也包括纯 C policy 与
data-definition record。Tenant-supplied CPU operation code 改为跨越单独版本化、绑定 attempt
的进程协议，该协议只传递有界 descriptor 与 shared-memory/FD capability；可信 Host 代码会
重新校验每个返回 descriptor 与 ownership 声明。纯 C 是 record compatibility 选择，不是
sandbox。

当前 Issue #102 切片通过源码私有的 Darwin/Linux protocol v1 实现其中的 transport 部分。
它只接受 Ready、Host-visible、未量化的 NativeScalar Strided DenseTensor value、scalar
parameter 与经检查的 dense-tensor output plan。全新的 one-call runtime 不接收 ABI pointer
record、Host callback、allocator、Graph/Run owner 或 resource token；其 process-local
callback seam 不会调用或迁移 operation ABI v2，也不会调用或迁移仍为目标态的 operation ABI
v1。直接使用仍保持 non-supervised，不提供 authentication、deadline、heartbeat、restart 或
有界 hang recovery。Issue #104 现在为该直接入口增加签名 package admission、Host resource
admission 与 exec 前 OS limit，但不提供通用 syscall/network sandbox。

Issue #103 现在通过源码私有的 `PluginRuntimeSupervisor` 与
`PluginInvocationExecutor` 实现受监督部分。专用 Unix datagram socket 上的定长 lifecycle
protocol 会把 OS 随机 128-bit nonce、完整 invocation identity、worker/plugin generation、
Host 选择的 heartbeat interval 与严格递增 event sequence 绑定到精确 exec PID。Startup、
invocation、heartbeat-gap、response、TERM、KILL 与 reap bound 都使用绝对单调 deadline。完整
request transfer 会获得一个独立、完整的 invocation-duration window；只有在全部 byte 与
descriptor right 已发送、Host `SHUT_WR` 成功，并且再次观察同一绝对 transfer deadline 后，
该传输才结束。通过该观察的精确单调时钟样本就是 `accepted_at`；callback invocation deadline
与初始 heartbeat-gap deadline 都直接从它派生。迟到但成功的 shutdown 是 invocation-deadline
fault，且不得启用全新 callback 或 heartbeat budget；shutdown 失败仍是 channel 事实。验收后
的调度停顿会消耗这些 budget，后续 caller 重新读取时钟不得再赠送一个窗口。构造阶段会在取得
child ownership 前，验证每个配置 duration 为正、未超过包含式 24 小时上限、可由 steady
clock 精确表示，并满足 heartbeat 字段顺序；它不会也无法验证未来的 base 求和。每次实际派生
deadline 都会在加法前以同一个已捕获 base 检查
`base <= time_point::max() - duration`。精确贴合上界会被接受。取得 ownership 前超出一个
tick 会通过受控异常 fail closed；取得 ownership 后，在 cleanup 前发生的 lifecycle 或短暂
精确 status-observation 溢出会映射为当前按阶段类型化的 fault 并执行精确 cleanup，而
派生 termination/reap-cleanup 或 restart-backoff deadline 时发生的算术失败会保留已经建立的
primary fault。如果这些 cleanup deadline 可以表示，但精确 PID 在最终 bound 时仍不可观察，
则唯一 ownership 会转移给 deferred reaper，`ReapPending` 会有意优先于更早的 phase fact。
Supervisor 绝不回绕、饱和、截断，也不会重新采样 base 来制造另一个窗口。如果没有更强的
process 或 deadline 事实，真实 channel/status-observation syscall failure 仍为 `Channel`。
Nonce 只证明私有 launch/session binding 与 liveness；由于 child 会知道它，该 protocol 不会
attest plugin truth，也不会让返回 byte 自动受信任。

Supervisor 会保留类型化可观测的 deadline、protocol、channel、bad-output、natural-exit、
signal 与 escalation 事实。它不会根据 wait status 推断 OOM：`SIGKILL` 只表示
memory-pressure-compatible。Fault 会关闭两条 channel，必要时升级精确 PID termination，保留
reap ownership，并且绝不回退到进程内执行。后续调用会等待有界 restart backoff，再启动另一
个全新进程。链接产品的真实 exec 测试会覆盖该 lifecycle。如果在认证 completion 仍排队时
精确 child status 已被回收，monitor 会先排空 lifecycle 队列，再把该 status 分类为未完成便
退出，随后继续保留 status 以执行强制的 response/EOF/decode/publication 检查。只有零退出绝不
授权成功。测试还会把 executor 组合进 `ExecutionService` ready callback；request boundary
只把 owning Run 发布为 Failed，固定 worker 随后会执行无关 Run。

Issue #104 现在会围绕两个长期维护的 isolated entry 组合 package trust 与 Host resource
authority。在 shared-memory、descriptor、socket、mapping 或 process 副作用前，无副作用 preflight
会导出五维 `PluginResourceVector`：runtime process、CPU slot、address-space byte、shared-memory
byte 与 descriptor count。Attempt-local `ResourceLedger` 会原子铸造 move-only token；该 token
绑定完整 invocation identity 的 domain-separated digest 与精确 vector。针对相同事实消费后会
生成一份 RAII lease；replay tombstone 在结算后仍保留到 ledger 生命周期结束，而未消费 token 或
已消费 invocation 的每条 success/failure path 都只归还一次精确 capacity。Invocation wire 不会
携带 token、trust root、manifest、signature 或 digest override。

已准入 vector 会在 native exec 前驱动 `RLIMIT_AS`、正 `RLIMIT_CPU`、经过检查的
`RLIMIT_NOFILE` 与零 `RLIMIT_CORE`。Child 仍只获得空 environment、`/dev/null` 标准流、固定
data/status/lifecycle descriptor、一个固定 executable descriptor 以及已声明 invocation
capability；该封闭集合以上的 descriptor 会被移除。Linux 使用 `fexecve` 执行复制后校验的
sealed descriptor。Darwin 对任何 native role 都没有受支持的无特权不可变 exact-object 边界，
因此 direct/supervised executor 构造期间会在候选访问、token 发放、capability materialization、
socket 创建或 fork 前报告 `ExactObjectUnsupported`；不会存在 runtime pathname snapshot。不支持
的平台（包括当前 Windows runtime profile）同样默认拒绝，不会回退到未经校验的 path。
这些控制施加 package identity 与 resource ceiling；它们不是通用 syscall/network sandbox，
观察到 `SIGKILL` 仍只表示 memory-pressure-compatible，不证明 OOM。

Adapter、endpoint、supervisor 与 executor 都会编入 installable product archive，但当前没有
operation loader 或 product composition root 会为最终用户 Graph operation 构造 isolated
request。具体而言，`ExecutionService`、`WorkerManager`、embedded Host/CLI 与
`photospider-worker` 均不存在 selection caller。Operation ABI v2 与仍为目标态的 v1 都不会
进入该 wire。Atomic operation-ABI migration、最终用户 selection 与更强 platform sandbox
profile 仍属于分别拥有的后续工作。Issue #106 现在会在既有 codec boundary 提供范围收窄的长期
evidence：一个手工 opt-in 的 Clang/libFuzzer target 会调用生产 isolated request/response
decoder 与 descriptor validator，而已注册的确定性测试负责 canonical roundtrip 和代表性的
malformed descriptor 行为。该证据不会打开 mapping、descriptor、callback 或 plugin process，
也不构成最终用户 route 或通用 sandbox 声明。

影子发布阻止操作注册表或策略类型 map 局部可见。组合 DSO 租约让回调状态和插件拥有
的值或上下文保持在其定义库的生命周期内，同时在 mapping 保持 resident 的整个期间保留精确授权
capability。最终释放会先 unmap DSO，再关闭 sealed descriptor。匹配的操作恢复 token 和策略绑定
代次可防止已移除或替换的插件静默夺回当前所有权。

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
- 数据定义 provider 包含 `photospider/plugin/data_provider_api.h`，请求
  `data_provider_sdk`，链接 `Photospider::data_provider_sdk`，并且只导出
  `ps_data_provider_get_abi_version` 与 `ps_data_provider_get_api_v3`。
- Definition provider 填写大小精确的记录，保留全部必须为零的 reserved 字段，
  让 callback metadata 存活到最终 generation destroy，并且绝不保留借用的 per-call view。
  C++ registry consumer 链接 `Photospider::operation_runtime`；这不意味着安装了 provider scanner。
- 仓库自有 OpenEXR provider 只能通过单独请求的 `openexr_deep_provider` component 使用。它
  export 相同两个 v3 symbol，绝不把 OpenEXR type 放入 public record，并且在默认 OFF 安装中
  完全不存在，包括 dependency discovery 与 target export。
- 策略插件包含 `photospider/policy/policy_plugin_api.h`、请求
  `policy_sdk` component，并链接 `Photospider::policy_sdk`。
- 策略插件导出精确的两个 v1 符号、填写大小精确的记录、保持每个 Host 初始化
  的前缀/保留字段，并且只返回已声明的状态/枚举值。
- 策略回调不保留快照内存，并把每个候选项 ID 当作不透明值。它们绝不创建工作
  线程，也不声称选择结果已经启动工作。
- 当前不存在 installed operation-v1 header、loader、SDK 或 dual-compatibility path。
  已接受 v1 目标只在完整 migration gate 后替代并删除 v2；scheduler SDK/ABI、
  `IScheduler` adapter 与执行路由插件 ABI 继续不存在。

## 实现与验证入口

- `include/photospider/data/value.hpp`
- `include/photospider/data/extension.hpp`
- `include/photospider/core/device.hpp`
- `include/photospider/data/image_view.hpp`
- `include/photospider/memory/access_plan.hpp`
- `include/photospider/memory/buffer_handle.hpp`
- `include/photospider/memory/ready_fence.hpp`
- `include/photospider/memory/strided_layout.hpp`
- `include/photospider/plugin/plugin_api.hpp`
- `include/photospider/plugin/data_definition_registry.hpp`
- `include/photospider/plugin/data_provider_api.h`
- `include/photospider/plugin/op_contract.hpp`
- `include/photospider/policy/policy_plugin_api.h`
- `src/lib/core/value.cpp`
- `src/lib/core/dense_tensor_content_digest.*`
- `src/lib/core/extension.cpp`
- `src/lib/core/cpu_dense_image_operation.*`
- `src/lib/plugin/operation_host_adapter.*`
- `src/lib/execution/device_completion.*`
- `src/lib/execution/isolated_cpu_invocation.*`
- `src/lib/execution/plugin_runtime_supervisor.hpp`
- `src/lib/execution/residency_manager.*`
- `src/lib/execution/value_transfer_task.*`
- `src/lib/execution/metal_device_executor.{mm,stub.cpp}`
- `src/lib/compute/progressive_compute.*`
- `src/lib/benchmark/i2_host.hpp`
- `src/lib/benchmark/i2_profile.*`
- `src/lib/benchmark/i2_evidence.*`
- `src/lib/plugin/plugin_loader.*`
- `src/lib/plugin/plugin_manager.*`
- `src/lib/plugin/data_definition_registry.cpp`
- `plugins/data/openexr_deep_scanline_provider.cpp`
- `src/lib/adapters/openexr/openexr_deep_scanline_adapter.*`
- `src/lib/policy/policy_registry.*`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_variable_sample_field_extensions.cpp`
- `tests/integration/test_openexr_deep_scanline_provider.cpp`
- `tests/integration/openexr_deep_provider_option_off_smoke.py`
- `tests/integration/dependency_disabled_install_smoke.py`
- `tests/integration/test_plugin_manager.cpp`
- `tests/unit/test_op_registry_m31.cpp`
- `tests/unit/test_policy_registry.cpp`
- `tests/integration/test_cpu_dense_tensor_image_operation.cpp`
- `tests/integration/test_metal_device_executor.cpp`
- `tests/unit/test_device_residency.cpp`
- `tests/fixtures/value_identity_dso.cpp`
- `tests/integration/test_value_identity_dso.cpp`
- `tests/unit/test_dense_tensor_content_digest.cpp`
- `tests/integration/static_product_consumer_smoke.py`
- `tests/integration/test_i2_product_path.cpp`
- `tests/integration/test_plugin_runtime_supervisor.cpp`
- `tests/unit/test_progressive_compute.cpp`
- `tests/integration/graph_cli_plugin_compute_smoke.py`
