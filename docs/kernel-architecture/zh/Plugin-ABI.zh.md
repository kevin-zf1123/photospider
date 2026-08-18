# 插件 ABI

Photospider 有三套独立版本化的已安装扩展 contract：operation ABI v1、data-definition
provider ABI v3 与 policy ABI v1。每套都是纯 C DSO 边界，并可选提供 header-only C++
authoring layer。Host 绝不让 C++ 标准库 object、exception、allocator、registry、owner 或
process-local runtime object 跨越这些边界。

英文架构文档与安装头是权威来源。本文描述已经实现的边界。历史 registration surface 已经
删除，不保留 dual loader、forwarding header、adapter shim、alias 或 missing-tail fallback。

## Operation Plugin ABI v1

### 已安装表面

完整的已安装 operation authoring surface 为：

- `include/photospider/plugin/operation_plugin_api.h`：self-contained、兼容 C11/C++17 的
  纯 C ABI；
- `include/photospider/plugin/operation_plugin.hpp`：构造相同 record，并把 plugin-local
  exception 隔离在 DSO 内的 header-only C++17 helper；
- CMake component/target `operation_plugin_sdk` /
  `Photospider::operation_plugin_sdk`；
- 可选 Value/runtime target `Photospider::operation_runtime`；
- 可选 OpenCV adapter component/target `operation_opencv` /
  `Photospider::operation_opencv`。

`operation_plugin_sdk` 没有外部 package 或 link dependency。纯 C 或只使用 header-only
helper 的插件只需要该 target。直接使用公共 Value/runtime helper 的插件显式链接
`operation_runtime`。OpenCV adapter 只发现 OpenCV `core`；算法所需的其他 module 仍由插件
自行声明。

每个 operation DSO 精确导出：

```c
uint32_t ps_operation_plugin_get_abi_version(void);

ps_operation_status_v1 ps_operation_plugin_get_api_v1(
    ps_operation_plugin_api_v1 *api);
```

数字 discovery 无副作用，必须精确返回 `PS_OPERATION_PLUGIN_ABI_VERSION`。Root discovery
填充 Host 预备的精确 96-byte v1 table。Root 暴露永久 plugin identity、受限
implementation-version bytes、一个不透明 round-trip generation context、suite query 与
exactly-once generation destruction。Reserved field 必须为零。

Host 请求精确的 64-byte version-one suite：

| Suite | 职责 |
| --- | --- |
| Definition | 枚举不可变 operation 与 implementation。 |
| Configuration | 验证 configuration，并创建/销毁 configured context。 |
| Inference | 发出由 Host 验证的不可变 output plan。 |
| Region | 传播 demanded/affected Region。 |
| Dependency | 在声明时发出受限 execution dependency。 |
| Execution | 执行 trusted monolithic 或 tiled callback。 |

任何 table 都不采用 minimum-size compatibility。Root、suite、semantic record、stride、
version、flags 与 reserved field 必须精确匹配已安装 v1 contract。未知 kind、非零 tail、
超限 count、无效 pointer/count pair 或不支持的 enum 都会在发布或 invocation 前失败。

### 精确 record 目录

所有 semantic record 都以 16-byte `ps_operation_record_header_v1` 开头，使用自然 8-byte
alignment，并携带精确 size。目录包含 30 个 record kind：

| Kind | Record | Size/alignment |
| --- | --- | --- |
| 1–7 | Diagnostic、OutputSink、ConfigurationNode/View、Operation/Implementation/PortDescriptor | 48, 48, 64, 48, 128, 192, 112 / 8 |
| 8 | `ps_operation_value_descriptor_v1` | 224/8 |
| 9–12 | FacetView、BufferView、ValueView、InputBinding | 64, 80, 128, 96 / 8 |
| 13–15 | OutputPlan、MutableOutputBinding、Invocation | 112, 128, 96 / 8 |
| 16–20 | RegionAtom、RegionSetView、RegionBinding、DependencyRecord、Tile | 96, 48, 80, 96, 64 / 8 |
| 21 | `ps_operation_dense_tensor_descriptor_v1` | 96/8 |
| 22 | `ps_operation_strided_layout_v1` | 64/8 |
| 23 | `ps_operation_image_facet_v1` | 160/8 |
| 24–25 | `ps_operation_channel_v1`、`ps_operation_channel_group_v1` | 48, 64 / 8 |
| 26–28 | ChannelSampleDomain、SampleDomainFacet、ColorFacet | 64, 80, 64 / 8 |
| 29–30 | OutputBufferPlan、OutputGrantSpan | 64, 64 / 8 |

Helper record 同样具有冻结布局：identity、generation、invocation identity、byte/mutable-byte
view、array reference、axis range 与 SHA-256 digest。每个 array reference 都有精确 element
stride、文档化 bound、alignment 与 pointer/count relationship。除非 record 明确规定
DSO-lifetime immutable metadata，否则每个 callback-local byte/record view 只在该次调用中借用。

ABI 将 rank 限制为 16；facet/buffer 为 64；channel、group 与 group member 为 4096；总 group
membership 为 65536；configuration node/depth/bytes 为 4096、64 与 1 MiB；Region atom 为 64；
dependency row 为 4096；output/grant span 为 1,048,576。Name、diagnostic、plugin version、
operation、implementation 与 port 也有安装 contract 中的上限。Host 每一道 fence 都必须使用
checked addition、multiplication、offset、extent、signed-window 与 stride arithmetic。

### DenseImage 与 generic Value 投影

`ps_operation_value_descriptor_v1` 保留精确 Schema、Facet、Layout identity、version 与 digest。
普通 DenseImage 指向：

- 一个 `DenseTensorDescriptor`，包含 rank、精确 `uint64_t` extent、element semantics、storage
  encoding，以及可选 quantization block shape 与 binary32 scale；
- 一个 `ImageFacet`，包含显式 x/y/可选 channel axis、signed data window、可选 signed display
  window、channel/group、可选逐 channel sample-domain override，以及可选 SampleDomain/Color
  facet；
- 一个物理 `StridedLayout`，包含 rank、buffer index、byte offset、signed byte stride 与精确
  storage span。

Channel/group 使用稳定 64-bit identity，诊断 name 是受限 byte view。Group membership 与逐
channel override 是 exact-stride array。Optional-record presence bit 是闭集；缺席 record 的
storage 具有 C 语义的全零值。ABI enum 为固定宽度且从一开始，零保留给缺席或无效状态。

`ValueView` 组合一个经过验证的 descriptor、受限 facet/buffer row 与 content/storage identity。
`InputBinding` 增加永久 port identity、dense slot、可选 edge identity、精确 logical Region 与
connected/disconnected 状态。Disconnected slot 保持显式，绝不允许 compaction 改变 port identity。

### Output planning 与 Host-owned grant

Inference 通过 Host sink 发出 `OutputPlan` record。Plan 命名 output port 与 Host mint 的不透明
plan identity，引用一个完整 value descriptor 与 full logical Region，并包含 exact-stride
`OutputBufferPlan` row。每行冻结 buffer index、access、offset、精确 size 与 alignment。Host 在
执行任何 allocation 前验证并深拷贝完整 plan。

Execution 只接收 Host 生成的 `MutableOutputBinding` record。每个 binding 回显已接受 plan、
binding identity 与 callback-scoped grant identity。其 `OutputGrantSpan` row 包含 checked
allocation offset、size、alignment、access mask 与 mutable CPU byte view。插件只能写入这些
span 以及当前 tile/Region 共同授权的 byte。

插件不会得到 allocator、seal callback、`ValueBuilder` 或可转移 owner。`OK` 加有效 sink/output
state 才会退役 grant 并允许一次 Host seal。非 OK status、首个 sink failure、exception、
cancellation、malformed echo、out-of-plan write、缺失 retirement 或 late result 都会让完整
binding fail closed。Host 不发布 partial output。

### Region、dependency 与 diagnostic sink

Region record 携带受限精确 atom，并保留 signed image window 或 generic tensor selection。
Backward propagation 把 demanded output Region 映射到 input/edge identity；forward propagation
把 changed input Region 映射到 affected output identity。Exact、whole、empty 与 unsupported
outcome 是闭集。Host 会在 planning 使用前 canonicalize、验证、深拷贝并限制每个发出 row。

Dependency record 命名 implementation、direction、input/output identity 与 Region relation，
但不暴露 scheduler、queue、Run、worker 或 resource grant。输出依赖数据的 implementation 必须
暴露 Dependency suite；缺失会让 registration 失败。

每个 callback 都可以通过 Host-owned sink 发出受限 diagnostic。首个 sink failure 是 sticky 且
具有权威性。Message 会同步复制到 Host-owned storage。Plugin-local C++ helper 会在 DSO 内捕获
`std::bad_alloc`、`std::invalid_argument`、其他标准 exception 与未知 exception，并返回冻结
status；exception object 不跨 ABI。

### Registration 与 publication transaction

进程 `PluginManager` 拥有一个 operation registry，并为每个 candidate 执行单一 transaction：

1. 在 native trust policy 下解析并授权精确 opened object；
2. 在确认 ABI v1 前只调用数字 discovery；
3. 在 Host storage 中预备精确 root 与 required suite；
4. 枚举受限 operation 与 implementation；
5. 验证每个嵌套 record、identity、relationship、callback、execution mode 与 runtime-package
   identity；
6. 深拷贝全部 definition metadata，并预备 callback/context owner；
7. 安装 sealed-object/native DSO 组合 lease；
8. 在 visible-registry lock 下不执行可能抛出的工作，原子发布全部私有 `OpRegistry` callback。

第 8 步之前任何失败都不会改变 visible registry。Definition identity 与 `(type, subtype)` key
必须唯一。已发布 callback 捕获精确 immutable generation、operation/implementation identity、
suite callback 与 DSO lease。Source-private C++ registry model 只是 Host projection，绝不是已安装
ABI。

每次成功 publication 都 mint revision 并保留 predecessor。替换 active definition 会 shadow
旧 generation，但 snapshot 仍持有它时不会销毁。卸载被 shadow 的 middle generation 会把其
predecessor splice 到更新 snapshot。Unload-all 按成功 publication 的逆序执行。调用 callback、
销毁 context/generation 或关闭 DSO 前都会释放 registry lock。

### Context 与 DSO lifetime

解析后的 implementation 可以从已验证 configuration snapshot 创建一个 configured context。
`OK` 加 null 是有效 stateless context。每次成功 create 都会使用相同
operation/implementation identity 与 context 收到精确一次对应 destroy；失败 create 不会收到
destroy。

只有所有 registry definition、configured context、output plan/result、callback snapshot 与
in-flight invocation 都释放后，完整 generation 才会收到一次 `destroy_plugin` 尝试。Destroy 在
精确 DSO lease 仍存活时运行，且绝不重试。Destroy failure 变为受限 Host-owned diagnostic，不会
回滚已提交 replacement。该 lease discipline 防止 callback、metadata 或 context 比代码 image
活得更久。

### Trusted 与 supervised CPU route

Implementation 精确声明一种 execution mode：

- `TRUSTED_IN_PROCESS`：Host 在 generation lease 下调用纯 C suite callback，并在 DSO 仍映射时
  捕获 foreign unwind；
- `SUPERVISED_PROCESS`：descriptor 包含非零不透明 signed runtime-package identity，Host 解析
  matching installed private `PluginInvocationExecutor` route。

Supervised mode 绝不包含或序列化 path、PID、descriptor、mapping address、callback、context、
pointer、native handle 或 in-process generation owner。缺失或不匹配 route 会在 direct callback
entry 前失败，且不存在 trusted fallback。

独立版本化的 isolation protocol 当前为 version 2。Request 携带 operation/implementation
identity、configuration、input descriptor/facet/layout/Region facts、不可变 output plan 与定向
shared-memory capability index 的 canonical bounded copy。Response 携带 status、受限 diagnostic、
plan echo、适用时的 Region/dependency row 与 written-range fact。Runtime 在 callback entry 前验证
framing 与 record。Host 解码为全新 bounded object，并在 seal/publication 前对照 immutable
request、当前 invocation/resource identity、output plan 与 Host grant 重新验证每个结果。

Unknown tail、duplicate row、hostile count/stride/reserved value、伪造 capability index、
out-of-plan range、lossy metadata 与带 pointer record 都 fail closed。既有 authentication、
deadline、fresh-process、resource-ledger settlement、fault classification、child reaping 与 recovery
语义仍由 `PluginRuntimeSupervisor` 与 `PluginInvocationExecutor` 拥有。

Darwin 会在 capability materialization 或创建 child 前刻意拒绝 exact-object supervised runtime
construction。Portable compile/layout 与 route-before-process failure test 在 Darwin 运行；native
exact-object DSO 与 supervised 正向执行仍是 Linux integration gate。

## Native Plugin Trust Admission

Dynamic code 在 plugin discovery 前完成 admission。Loader 打开并授权稍后映射的同一个 object，
验证配置的 trust bundle 与 signature/digest policy，拒绝 writable 或被替换的 candidate，并把
sealed-object identity 带入 lifetime lease。Trusted in-process plugin 仍是具有进程 authority 的
native code；纯 C 提供 binary compatibility 与 validation，而不是 sandbox。

Operation、policy 与 data-definition contract 共用该 admission model，但保留不同 ABI number、
entry symbol、root、suite、registry 与 authority。一个 family 的失败绝不会启用另一个 family 的
entry point。

## Data-Definition Provider ABI v3

Data-definition provider 通过下列入口发布 immutable Schema、Facet 与 Layout definition bundle：

```c
uint32_t ps_data_provider_get_abi_version(void);
ps_data_provider_status_v3 ps_data_provider_get_api_v3(
    ps_data_provider_api_v3 *api);
```

已安装 dependency-neutral component 是 `data_provider_sdk`，target 是
`Photospider::data_provider_sdk`。Provider callback 验证和观察 pure property、DataSpec/Region
relationship、canonical content 与 generation lifetime。它们不会得到 allocation、conversion、
execution、device、Graph、registry mutation 或 codec authority。Host-side C++ registry 用户另行
链接 `Photospider::operation_runtime`。

OpenEXR deep support 是可选 provider/adapter family。只有其显式 component 才发现 OpenEXR 3；
neutral SDK/runtime component 绝不会发现它。Provider 缺席时未知 provider byte 仍按 byte 保留，
generation-owner lease 会在 retained traversal 期间保持 provider code 存活。

## Policy Plugin ABI v1

Policy DSO 精确导出：

```c
uint32_t ps_policy_plugin_get_abi_version(void);
ps_policy_status_v1 ps_policy_plugin_get_api_v1(
    ps_policy_plugin_api_v1 *api);
```

已安装 dependency-neutral component 是 `policy_sdk`，target 是
`Photospider::policy_sdk`。Callback 接收受限 immutable candidate/ranking snapshot，并返回一个
candidate identity 或 abstention。它们不会得到 worker、queue、resource grant、executor、Run、
Graph、allocator、completion route 或 lifecycle authority。

Policy load 使用 staged + atomic publication。Binding 与 in-flight ranking snapshot 保留精确 DSO
generation。首个 callback fault 保持稳定，diagnostic 属于 Host；所有 holder 释放后，unload 与
destruction 在 registry lock 外执行。

## Compatibility 规则

- ABI family 独立版本化；operation 变化不会重新编号 provider、policy、IPC、worker 或 durable
  format。
- 必须使用精确当前 size。不接受更小 prefix、missing tail、alias、wrapper、dual loader 或
  forwarding compatibility。
- Entry point、calling convention、固定宽度 enum、count、bound、alignment 与 reserved-zero
  field 都属于 binary contract。
- 任何 owning ABI version 变化后，插件都必须针对 matching installed SDK 重新构建。
- Process-local pointer 只能在授权的 in-process callback 内存在，绝不跨 isolation wire，也不成为
  durable identity。
- DI-4 仍负责最终迁移 operation/isolation 边界以外的 public Host、IPC/worker、durable、codec、
  CLI 与其余 `ImageBuffer` surface。

## 实现与验证入口

主要实现入口：

- `include/photospider/plugin/operation_plugin_api.h`
- `include/photospider/plugin/operation_plugin.hpp`
- `src/lib/plugin/plugin_loader.cpp`
- `src/lib/plugin/operation_host_adapter.hpp`
- `src/lib/plugin/operation_host_adapter.cpp`
- `src/lib/plugin/operation_runtime_router.hpp`
- `src/lib/plugin/operation_runtime_router.cpp`
- `src/lib/execution/isolation/isolated_cpu_invocation_protocol.hpp`
- `src/lib/execution/isolation/isolated_cpu_invocation_protocol.cpp`
- `src/lib/execution/device/plugin_runtime_supervisor.hpp`

长期验证入口包括独立 installed C11/C++17 consumer、精确 layout assertion、hostile
root/suite/record fixture、仓库 operation/OpenCV provider 行为、output-plan/grant/Region/dependency
test、replacement 与 middle-generation lifecycle test、in-flight DSO 与 destroy-once test、isolation
protocol-v2 round trip、hostile response test、route-before-process fail-closed 行为，以及受支持平台
上的 supervised runtime test。Migration residue scan 仍是本地开发检查，不注册为 CTest 或 CI
行为测试。
