# ADR 0008：通用 Value、内存绑定与 Region 是显式的版本化契约

## 状态

已接受为 Project 4 通用数据与异构执行的目标契约。源码树现在已经实现有界的 V-2 至 V-4
子集：CPU DenseTensor/ImageView Value、checked BufferHandle ownership 与 runtime identity，
以及由 dirty planning、validity 和 core dense operation 使用的 public Region MVP。
`ImageBuffer`、`DataType`、`Device`、`ParameterMap` 与 operation plugin ABI v2 仍是各自
角色边缘上的兼容契约；本 ADR 中尚未实现的部分仍是演进目标。

Issue #78 批准了本契约。Issue #79 至 #81 交付了有界的 V-2 至 V-4 实现切片；Issue #82
至 #90 仍是彼此独立的实现切片。合成的
`VariableSampleField` 证明与可选 OpenEXR Deep provider 仍是彼此独立的后续 change；
本决策不实现二者。

## 背景

当前 `ImageBuffer` 契约是有用的二维图像 payload，但不能通过持续追加字段而扩展成通用图
value model。逻辑含义、物理存储、设备访问、就绪状态、缓存身份与 Region 推理具有不同的
owner 和版本生命周期。混合这些事实会让存储搬移改变逻辑身份、让单个 device enum 暗示
可访问性、把 padding 变成内容身份的一部分，并强迫未来数据族进入一个封闭枚举。

目标需要支持同构 rank-N Tensor、任意通道图像、sub-byte 与量化 encoding、
provider-defined Layout、多种内存域、异步 producer completion、有界逻辑 Region、
structured value 与每个 site 可变的 sample 数量。目标必须保留未知但有效的 extension，
保持依赖中立，并允许 provider generation 在仍有 in-flight Value、lease、query 与 callback
时安全 retire。

本决策还必须保留既有所有权决策：

- [ADR 0003](0003-process-owned-execution-resources.zh.md)与
  [ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
  继续把物理执行资源、admission、ready work 与 provider lifecycle 放在注入的进程执行域。
- [ADR 0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.zh.md)
  继续区分当前事实、已接受决策、演进目标与 live 交付状态。
- [ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md)继续要求可选库位于依赖中立的
  provider 与 adapter 边界之后。

## 决策

### 逻辑 descriptor 与物理 binding 分离

每个具体 `Value` 都有一个不可变 `DataDescriptor`。Descriptor 只包含逻辑语义：

- 精确一个版本化 `RepresentationSchema` identity 及其 canonical payload；
- 零个或多个版本化 Facet identity 及其 canonical payload；以及
- 解释逻辑对象所需的具体逻辑 dimension、element semantics、channel、time、domain 与其他
  schema-defined fact。

它不包含 allocation、plane、stride、byte offset、packing、device、mapping、fence 或 native
handle。物理事实使用单独的组合：

```text
StorageBinding =
  StorageLayout
  + BufferHandle[]
  + ReadyFence
  + AccessProvider lease
```

只有 producer 声明多个 binding 在同一逻辑 revision 下等价时，一个 `Value` 才能拥有多个
authoritative binding。Residency 或较低访问成本本身不会使 binding 成为权威。Core 验证有界
envelope 与交叉引用；匹配的 provider 验证 Schema、Facet、Layout 和 access 的特定不变量。

### Value 语义、构造与生命周期

`Value` 是通过 PImpl 实现的 final、可复制、不可变 RAII handle。复制会共享不可变 control、
allocation owner 与 provider-generation lease，不会复制 payload byte。Consumer 不能继承
`Value`，也不能替换其 descriptor、binding、readiness、revision 或 lease。

`ValueBuilder` 只能 move，并且拥有正在构造的存储的唯一普通写权限。它可以放弃构造，或精确
seal 一次。成功 seal 会：

1. 证明 descriptor 已完全具体化；
2. 验证 descriptor、Layout、handle、fence、access 与 provider envelope；
3. 创建新的进程本地 `ValueRevisionId`；
4. 原子撤销 builder 或 caller 持有的每个 `WriteLease`，以及能够取得写权限的每条公共或
   consumer 路径；以及
5. 在完成下述 producer 权限转换后发布不可变 `Value`。

Seal 是一次原子写权限转换。如果 `ReadyFence` 已经 terminal，全部 producer 写权限都必须在
seal 前停止访问存储并退役。如果 fence 仍为 Pending，seal 会把唯一排他写权限转交给私有的
producer-scoped write capability。只有已登记的异步 producer，或代表该 producer 执行的
native owner，可以保留这一 move-only capability；其生命周期与 producer 的 terminal
fence transition 耦合。该 capability 绑定精确的 `ValueRevisionId`、
provider-generation lease，以及预先验证的
`StorageBinding`/Layout/`BufferHandle` envelope。它只能在该 envelope 内完成此前
已经承诺的 payload 写入；不得改变 descriptor、binding 集合、Layout、allocation/native owner
或 revision。它绝不公开、不可复制，consumer 也无法取得。

Payload 仍可在 `ReadyFence` 后处于 pending；descriptor 不得 pending。Pending 只表示已登记
producer 正通过其受限 capability 完成已承诺的 payload，不会使 sealed `Value` 变成一般可写。
`ImageView` 与 `DenseTensorView` 是经过检查的 final facade，并保留完整 `Value`。构造时要么
验证必需的 Schema 与 Facet，要么返回 typed failure。它们绝不借用可能失去 owner 的裸
descriptor 或 allocation。

`StructuredValueSchema` v1 是自包含的。它的 descriptor 可以递归描述字段，但一个 v1
`Value` 不包含 runtime child `Value` object。独立结果使用命名 output port 或不带 identity 的
`ValueBundle`。可共享的 `CompositeValue` DAG、跨 Value identity、cycle 与 graph persistence
需要单独决策。

### Representation Schema、Facet 与 extension identity

不存在封闭的 `ValueKind` 枚举，也不存在通用的 optional property bag。
`RepresentationSchema` 定义逻辑表示，例如 DenseTensor、VariableSampleField 或
StructuredValue。正交的版本化 Facet 添加 image、Deep sample、color、alpha 或 time 含义。

每个 Schema 与 Facet definition 都具有：

- 永久的 128-bit `SchemaId` 或 `FacetId`；
- 只用于诊断、永不作为权威 identity 的名称；
- 独立 structural version `{major, minor}`；
- canonical 的有界 byte representation；以及
- 适用的 validation、query、region、digest、migration、conversion、inference 与 execution
  hook。

Provider 会发布显式支持的 version set 或 range，以及显式 migration。无关 identity 使用相同
major number 不代表兼容。当有界 envelope 有效而 provider 缺失时，系统可以逐 byte 精确保留
未知数据，但不能解释、规范化、重新计算、转换、进行 Region 求值、计算 canonical content
hash 或执行该 extension。

稳定 core 只知道 envelope framing、永久 identity、structural version、通用内存边界、
canonical payload framing、公共 query/result 类型与 registry protocol。它并不知道未来全部
Schema、Facet、Layout、device、conversion 或 kernel。

### DenseTensor、图像、可变 sample 与 structured value

DenseTensor 表示同构逻辑元素构成的 rank-N 矩形集合。它不暗示 NCHW/NHWC 顺序、image axis、
channel、color、alpha 或 media time。Runtime descriptor 具有具体 rank 与 extent；包含 symbolic
或 unknown runtime dimension 的 descriptor 不能 seal。`DataSpec` 而不是 `DataDescriptor`
可以携带 symbolic dimension 与 constraint。

普通任意通道图像表示为：

```text
DenseTensor + ImageFacet
```

`ImageFacet` 把有符号 half-open x/y coordinate 与可选 channel axis 映射到底层
representation。它不选择 planar 或 interleaved storage。有符号 `ImageBounds` 可以表示非零或
负的 data-window origin。

`ChannelSchema` 支持任意 channel 数量、稳定 `ChannelId` 与显式 `ChannelGroupId` group。
名称只用于诊断，绝不暗示 color、alpha、depth 或其他 role。`ColorFacet` 与 `AlphaFacet`
把语义绑定到显式 group 或 channel。它们绝不从 `R`、`G`、`B`、`A` 等名称推断 role；
conversion 会产生新的 `Value`。

`TimeFacet` 具有 `TimeDomainId`、`TimeBase`、有符号整数 tick 与 half-open `TimeRange`。
Nominal rate 在精确时使用 rational 表示，否则必须显式标为 approximate。Variable-frame-rate
timestamp 始终具有权威性。

更多 channel 不会使 image 变成 Deep。每个逻辑 site 可具有可变 sample 数量时使用
`VariableSampleField`。OpenEXR Deep 逻辑 value 表示为：

```text
VariableSampleField + ImageFacet + DeepSampleFacet
```

异构 channel 或独立采样 channel 使用 multi-plane structured representation 或版本化
extension，而不是破坏 DenseTensor 的同构性。

### Element 含义、encoding 与 quantization

目标不再把 `DataType` 作为通用数据契约，而是拆成三个独立概念：

- `ElementSemantics`：逻辑 domain 与解释；
- `StorageEncoding`：bit width、lane、packing、byte/bit order 与物理 encoding rule；以及
- `QuantizationSchema`：scale、zero point、block/group axis、calibration 与
  schema-defined quantization parameter。

支持程度分别归类为 describable、executable 与 convertible。Describable 表示 descriptor 与
storage envelope 有效；executable 表示选定 kernel 支持所请求 capability；convertible 表示
存在显式注册的 conversion。任何路径都不得静默改变含义、encoding、quantization、
channel role、color、alpha 或 time。显式 conversion 会创建新的 `ValueRevisionId`。

### Buffer handle、Layout、lease 与写安全

Allocation control block 拥有 native allocation identity、provider/device state、release
behavior 与 lifetime。`BufferHandle` 是 allocation 中已检查 byte range 的不可变、可复制 view。
它既不包含通用 raw pointer，也不包含可复制的 mutable flag。

一个 binding 可以使用多个 handle；只有 Layout 与 access rule 允许时，handle 才能引用互不相交
或重叠的 range。Core validation 使用 checked arithmetic 证明每个声明的 envelope 都位于其
handle 内。

公共 builder 与 consumer 的 payload access 要求：

- `ReadLease`：保留 allocation、provider generation、mapping/import state、visibility
  obligation 与不可变访问；或
- `WriteLease`：还要证明 exclusive builder authority 与有效、无重叠的 writable Layout。

Pending producer-scoped write capability 是 seal 时对普通 lease access 的唯一例外。它携带
相同的 checked、non-overlapping envelope 证明，但只属于已登记的 producer 或代表该
producer 执行的 native owner，不能通过 sealed `Value` 请求。

已 seal 的 `Value` 不能发出 `WriteLease`，并且 consumer writable access 始终被拒绝。Direct
CPU pointer、mapped pointer、imported resource 与 transfer destination 只存在于对应 lease
或私有 producer capability 和 access-provider contract 内。

首批 core Layout family 是：

- `Strided`：byte offset、与 shape 兼容的 plane mapping 和有符号 byte stride；
- `Blocked`：显式 block/tile geometry 与 packing；以及
- `ProviderDefined`：版本化 opaque payload 加强制通用有界 buffer envelope。

Strided read 可以使用正、负或零 stride。负 stride 与零 stride 在 v1 中只读。Writable access
必须证明所请求写 Region 的 address 位于边界内且互不重叠；无法证明时拒绝 lease。
Provider-defined Layout 不能绕过 core bounds，必须列出可能访问的每个 handle 与 byte
envelope。

### Device identity、access、residency、readiness 与 visibility

目标拆分：

- `DeviceBackend`：稳定 backend family，例如 CPU、CUDA、Metal 或 Vulkan；
- `DeviceId`：某个具体 device 的进程本地 identity；以及
- `MemoryDomain`：版本化 allocation domain，例如 host、pinned host、device-local、
  shared 或 imported memory。

这些事实都不暗示可访问性。`AccessProvider` 返回显式 `AccessPlan`：

```text
Direct | Map | Import | Transfer | Unsupported
```

Plan 标识 source binding、target capability、必需 readiness、visibility work、lease、
resource demand 与最终 binding 或 access scope。Device、Layout、memory-domain、residency 与
access constraint 属于 `KernelCapability`，不属于 `DataSpec`。

`Value` 拥有 authoritative binding。进程拥有的 `ResidencyManager` 可以拥有按
`ValueRevisionId` 索引的额外 replica；这些 replica 既不会序列化进 `Value`，也不会被视为
权威。Stale replica 不能满足更新的 revision。

`ReadyFence` 是不可变、可复制的 observer；`FenceCompleter` 是只能 move 的 terminal
publication capability，本身不授予 payload write access。精确允许一次 terminal transition：

```text
Pending -> Ready
        -> Failed
        -> ProducerCancelled
```

丢弃 unresolved completer 会发布 `ProducerCancelled`。`poll` 不阻塞。Wait 会通过所属
execution 或 device mechanism 异步调度；CPU worker 不会阻塞等待 device fence。取消某个
waiter 不会修改共享 fence，也不会取消 producer。

在发布 Ready、Failed 或 ProducerCancelled 前，producer 必须停止全部 payload access，并释放
或撤销 producer-scoped write capability。全部 producer 写入与该 capability 的退役
happen-before observer 可以看到 terminal transition。在 capability 仍可访问存储时发布
terminal state 属于无效行为。

`Ready` 只报告 producer completion。它不建立 host mapping、cache coherency、queue-family
ownership 或 consumer visibility；这些职责属于 `AccessPlan`。Pending、Failed 与
ProducerCancelled 允许访问不可变 metadata 与 diagnostic，但不能产生 consumer `ReadLease` 或
payload-visible access。Ready 之后，consumer read 仍必须由选定 `AccessPlan` 完成其 visibility
obligation，之后才能签发 `ReadLease`。Fence、pending wait、access scope、私有 producer
capability 与 native owner 都保留定义它们的 provider-generation lease。

`ComputeRun` 会保留 request-local 不可变 Value 及其 authoritative binding。Settlement 会核算
每个 output 的 terminal fence state、provider-generation lease、access obligation 与
ResourceLedger release。Failed fence 可以让 Run 以 typed failure settle 并释放 request
accounting；同时 callback、wait 或 access lease 继续让 retiring provider 与 native owner
存活。关闭 Run 会释放其 Value handle，但既不会销毁 in-flight owner，也不会阻止符合条件的
进程级 residency replica 按精确 `ValueRevisionId` 保留。本规则不会把 Run、dispatcher、
ledger、commit 或 shutdown authority 从 ADR 0007 定义的 owner 移走。

### 有界逻辑 Region

`RegionSet` 是基于显式 `RegionDomainKey` 的有界、可扩展析取范式：

- 同一个 clause 内的 atom 按 AND 组合；
- clause 之间按 OR 组合；并且
- 每个 atom 都标明自己的逻辑 coordinate domain。

`Whole` 是一个空 clause，`Empty` 是零个 clause。Interval 是 half-open。MVP 支持 Whole、
Empty、`ImageRect`、`TensorSlice`，并且最多有一个 nonempty clause。其他 atom type 与多个
clause 是版本化 extension。

Union、intersection、difference、projection 与 transformation 会消耗显式 complexity budget，
并返回以下一种结果：

```text
Exact(RegionSet)
ConservativeSuperset(RegionSet, reason)
Unknown
Unsupported
TooComplex
```

后三者是 outcome，而不是伪造的 Region。Hull 或 Whole fallback 只有在显式标为
`ConservativeSuperset` 时才合法；每个 caller 自行判断该近似是否适用于 planning、
invalidation 或 execution。

已实现的 V-4 子集安装了该 value/algebra 契约：固定的内建 image 与 dense-tensor domain
key、signed 64-bit `ImageRect` interval、rank-general unsigned 64-bit `TensorSlice`
interval、单个 nonempty clause 最多八个 atom 的硬上限，以及显式 caller budget。Dirty
source fact、per-node affected work、edge mapping、HP/RT validity 和 core dense operation
都保留规范化 `RegionSet`。当前 image tiling、ImageBuffer helper、Host/IPC inspection 与
operation ABI v2 仍保留 checked derived `PixelRect` projection。RT 只支持 image；
TensorSlice 是 HP monolithic work，绝不被重新解释成二维 geometry。

### DataSpec、capability、property 与 output inference

`DataSpec` 描述一组可接受的具体 descriptor。它可以包含 symbolic dimension、range、
Schema/Facet version constraint、element/quantization predicate 与 provider-evaluable
condition。

对于 producer set `P` 与 consumer set `C`：

- `P` 是 `C` 的子集时静态兼容；
- 二者交集为空时不兼容；
- 非空的部分重叠需要显式 runtime guard；并且
- 缺少 provider logic 时得到 `CannotEvaluate`。

兼容性绝不插入 conversion。需要 conversion 时，由 graph planning 选择显式 conversion
operation。

纯、非阻塞 property query 精确返回一个 `PropertyQueryResult<T>` state：

```text
Available(T)
NotApplicable
Unknown
Deferred
MissingProvider
UnsupportedSchemaVersion
InvalidDescriptor
```

Query 不执行 IO、payload access、mapping、transfer、allocation-sized compute 或 device work。
解析 `Deferred` 所需的工作通过显式 operation 或 scheduled prepare step 表示。

Output 描述分成三个阶段：

1. static inference 把 input `DataSpec` 与 configuration 映射成 output `DataSpec`，不读取
   payload；
2. 纯 concrete inference 把具体 descriptor 与 configuration 映射成
   `Exact(DataDescriptor)` 或 `Deferred`；以及
3. scheduled prepare/execute 执行依赖 content、IO 或 device 的工作。

Operation 可以在 descriptor deferred 时启动 scheduled work，但 output descriptor 具体化前
不能 seal。已 seal output 可以在 fence 后处于 payload-pending。命名 operation output 是命名
`Value`；迁移后 `ParameterMap` 只用于 configuration。

### Error model 与 provider ABI

Query 使用上述 typed observation state。可能失败的 operation 使用 `Result<T>` 或 `Status`，
具有稳定 category 与自有 diagnostic。Pure-C provider callback 和异步 device/IO completion
绝不传播 C++ exception。Provider boundary 在保留 provider lease 的同时，把资源耗尽转换成
`resource_exhausted`，把未知 exception 转换成 Host 自有 internal error。

Schema、Facet、Layout、access、conversion、inference、query、region、digest 与 execution
provider suite 使用单独版本化的 pure-C provider ABI v3。Record 使用 fixed-width scalar、
显式 size/kind/version、有界 byte/string view、opaque handle、Host 自有 output storage、
status return 与必须清零的 reserved field。它们不暴露 STL type、exception、RTTI object、
virtual class、allocator owner、`Value` PImpl、native owner reference 或可变 Host registry。

C++ SDK 在这些 suite 上提供 RAII wrapper，而不改变其 wire layout 或 authority。实现前必须
冻结并独立复现精确 record layout、limit、calling convention 与 callback inventory。

注入的进程执行域拥有彼此独立的 Schema、Facet、Layout 与 provider registry。它们不是 global
或 function-static singleton。对于一个精确 identity 与 structural version，最多有一个 active
definition provider；多个 execution kernel 可以在不同 capability 上支持同一个逻辑 definition。

已发布 definition 与 kernel binding 是不可变 generation owner。Caller 在 validation、query、
inference、access planning、invocation、result conversion 与 provider-created owner lifetime
期间保留 generation lease。Replacement 会准备完整 candidate、原子发布，并让旧 generation
经过：

```text
Active -> Retiring -> Unloaded
```

Retiring 拒绝新 lease，但在全部既有 lease 与 provider-created owner retire 前保持 mapped。
Preparation 或 publication 失败会保留先前 generation。

Policy plugin C ABI v1 独立版本化，不会被改名为 v3。ADR 0007 定义的进程执行域、
ResourceLedger、ready store 与 policy authority 保持不变。

### Runtime identity、digest、persistence 与 cache authority

每次成功 seal 都会创建新的进程本地 `ValueRevisionId`，即使 descriptor 与 content 等同于更早
的 value。持久且可比较的 identity 保持分离：

- `DescriptorDigest`：canonical logical descriptor envelope；
- `ContentDigest`：Schema 定义的 canonical logical content stream；
- `StorageLayoutDigest`：不包含 native allocation identity 的 canonical physical Layout
  description；以及
- `ArtifactId`：某个持久 manifest 或 artifact version 的 identity。

每个 digest 都携带 algorithm tag。`ContentDigest` 可以是 `Deferred`。它排除 device identity、
allocation identity、fence、padding、physical stride 与 replica state。Schema provider 定义
canonical traversal，使等价逻辑 value 在允许的不同物理 Layout 上得到相同 hash。

Persistence 分成四层：

1. graph document 包含 operation configuration、命名 port 与 `DataSpec` constraint；
2. descriptor envelope 包含 Schema/Facet identity、structural version、canonical payload 与
   unknown extension byte；
3. artifact/cache manifest 包含 descriptor/content/layout digest、`ArtifactLayout`、
   content-addressed chunk reference、codec/provider metadata 与 `ArtifactId`；以及
4. runtime state 包含 binding、handle、native/allocation/device identity、fence、lease、
   access plan 与 residency replica，并且绝不持久化。

Schema、Facet、Layout、provider ABI、graph document、artifact manifest、codec 与 digest
algorithm version 是彼此独立的轴。未知但有效的 extension payload 会在 descriptor 与 artifact
读写循环中逐 byte 保留；provider 缺失时不会规范化它。

当前正式 HP cache 与 RT transient state 的所有权继续有效，直到后续实现切片迁移它们。
目标不允许出现第二个 cache authority，也不把 residency replica 提升为逻辑或持久 authority。

### 不保留永久 shim 的完整 public migration

目标安装 surface 组织在：

```text
include/photospider/core/
include/photospider/data/
include/photospider/memory/
include/photospider/plugin/
```

最终替换关系是：

```text
ImageBuffer     -> Value + ImageFacet + ImageView
PixelRect       -> RegionSet atom ImageRect
Device          -> DeviceBackend + DeviceId + MemoryDomain
OperationOutput -> named Value outputs
ParameterMap    -> configuration only, never a data payload
```

仓库自有 operation 与 provider 首先迁移。随后按依赖顺序迁移自有 adapter、cache、graph
document、Host value、CLI/IPC translation、test、installed consumer 与文档。只有全部自有
operation plugin 与 consumer 都有 v3 replacement 后，才删除 operation ABI v2、其 entry point、
SDK、fixture 与 package surface。

最终状态不存在永久 compatibility wrapper、alias class、重复 old/new API、forwarding header、
dual loader、v2-to-v3 shim 或双重 descriptor/cache/ABI authority。Temporary edge adaptation
只能存在于显式限定的实现切片内，并且必须在该切片的 completion boundary 删除。

### 验证边界

Issue #78 只修改架构与文档。

首条实现链仍是 Issue #79 至 #90。每个 Issue 都是可独立测试的纵向切片，并且必须消费本契约，
不能静默缩窄契约。

后续依赖中立的合成 `VariableSampleField` 切片跟踪为 V-14。它必须在不依赖 OpenEXR 的条件下
证明 registration、unknown byte preservation、descriptor 与 Layout validation、multi-buffer
binding、Region/DataSpec/query behavior、canonical digest、generation lease、hot replacement
与 unload。

另一个独立的可选 OpenEXR 切片跟踪为 V-15，首期只支持 single-part deep-scanline read/write。
它映射到 `VariableSampleField + ImageFacet + DeepSampleFacet`，并让 OpenEXR type、header、
link、symbol、codec/IO 与 package requirement 只属于 provider。Deep tiled、multipart 与混合
shallow/deep part 仍是后续工作。关闭 option 时，kernel、public ABI、dependency-disabled
product 与 transitive install dependency 中都不存在 OpenEXR。

## 结果

- 逻辑 identity 不随存储搬移、重新 packing、transfer 或额外 residency 改变。
- 未知但有效的 extension 可以被保留，而不会伪装成可执行或可查询。
- 内存安全、exclusive write、readiness、visibility 与 provider lifetime 变成显式契约，不再只是
  raw pointer 周围的约定。
- Region 与兼容性的不确定性保持为 typed outcome；caller 不能静默替换成 Whole、hull 或
  conversion。
- Pure-C provider ABI 与 generation lease 允许受控 replacement，而不跨 DSO 暴露 C++ layout
  或 exception ownership。
- 设计增加了显式 Schema、Facet、registry、lease 与 result state。接受这些复杂度，是因为被
  拆出的内容是正确性与生命周期边界，而非可选 metadata。
- 未实现的目标行为不会仅因本 ADR 接受它就变成当前事实；每个后续切片都必须同步更新 code、
  长期 test、当前事实文档与 installed contract。

## 被拒绝的替代方案

### 持续扩展 ImageBuffer 与 DataType

拒绝，因为 rank、variable sample、sub-byte packing、quantization、逻辑含义、storage 与 device
access 有彼此独立的不变量和版本轴。

### 使用带 optional field 的单个 ValueKind enum

拒绝，因为 extension identity 会依赖 core release、无效字段组合会不断增加，而且 unknown
payload 无法忠实保留。

### 把 Device 或 residency 当成可访问性

拒绝，因为 backend family、具体 device、allocation domain、visibility 与实际 consumer access
是不同事实。

### 只保存一个 bounding PixelRect 并静默放大

拒绝，因为这会丢失 Tensor 和稀疏逻辑 domain，并向 caller 隐藏 approximation。

### 复用 operation ABI v2 并传递新的 C++ Value object

拒绝，因为 C-linkage entry symbol 不能稳定 C++ layout、allocator、exception、RTTI、
standard-library ownership 或 toolchain ABI。

### 把 OpenEXR 作为第一项扩展性证明

拒绝，因为 codec dependency 可能掩盖 core registry、persistence 与 memory-envelope 缺陷，
也会违反依赖中立基础。

## 与当前事实和演进目标的关系

下列维护中文档仍是当前行为的权威来源：

- [内核数据模型](../../kernel-architecture/zh/Data-Model.zh.md)；
- [ImageBuffer 内存契约](../../kernel-architecture/zh/ImageBuffer-Memory-Contract.zh.md)；
- [插件 ABI](../../kernel-architecture/zh/Plugin-ABI.zh.md)；以及
- [内核缓存模型](../../kernel-architecture/zh/Cache-Model.zh.md)。

[通用数据与 Region 路线图](../../roadmap/zh/Kernel-Evolution.zh.md#通用数据与-region)是已接受
目标和实现依赖顺序的权威来源。Live Issue 与 Project state 仍是交付状态的权威来源。本 ADR
和路线图都不会把未实现的目标对象提升成当前 runtime 文档中的事实。
