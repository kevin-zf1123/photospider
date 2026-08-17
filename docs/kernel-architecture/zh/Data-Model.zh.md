# 内核数据模型

本文档描述当前内核使用的图和节点数据结构。`GraphDefinition` 是脱离运行态的持久文档 value；
`GraphModel` 与 `Node` 是私有 backend runtime state，不是共享 public contract。Frontend 使用
`ps::Host` value，operation plugin 使用 operation SDK，scheduler 只接收 ready-task metadata。
本文说明这些边界最终操作的内部行为。

## GraphModel

`GraphModel` 是图的内存状态。每个 `GraphRuntime` 拥有一个 `GraphModel`。

重要字段：

| 字段 | 含义 |
| --- | --- |
| 私有节点存储 | 从节点 id 到 `Node` 的映射，通过 `GraphModel` 查找、遍历和变更 helper 访问。 |
| 拓扑邻接索引 | 面向图像边和参数边的 incoming/outgoing `GraphTopologyEdge` 映射，以稳定节点 id 为键。 |
| `cache_root` | 当前图磁盘缓存文件的已解析根目录。 |
| `timing_results` | 启用计时时的最新计时摘要。 |
| `total_io_time_ms` | 累计磁盘缓存 IO 时间。 |
| disk-cache diagnostic snapshot | 最近一次磁盘缓存加载诊断，包含 skipped/miss/hit/error 状态，并在读取失败时包含错误详情。一个私有 diagnostic store 独占 optional value 与一把 no-throw mutex；record、snapshot、clear/reload reset、compute clone 与 staged publication 全部经过该 store，读取方只取得独立值快照。 |

外部代码不得通过原始节点 map 改变图结构。读取使用 `node()`、`find_node()`、`node_ids()` 和受控遍历等 helper。结构变更使用 `add_node()`、`replace_node()`、`remove_node()` 和输入重连 API；这些 helper 会在返回前验证并刷新拓扑邻接。节点本地运行态缓存/状态更新可以使用 `mutable_node()`，但结构编辑仍属于模型变更 helper。

内部服务通过模型边界协调锁、计时、缓存、拓扑和遍历行为。frontend、CLI 与 TUI code
通过 public `ps::Host` seam 访问图状态；embedded Host adapter 再委托给内部
`InteractionService`/`Kernel` 边界。backend service 可以使用该内部边界，但不会把它暴露给
frontend caller。

对于 CLI 加载的 graph，`cache_root` 会从已加载配置中的 `cache_root_dir` 推导为
`<cache_root_dir>/<graph_name>`；相对路径按进程当前工作目录解析。未提供 cache root 的底层
`Kernel::load_graph` 调用继续使用 session-local fallback：`<root_dir>/<graph_name>/cache`。

`GraphModel::clear()` 会重置模型级运行时状态，而不只是删除节点。清理图会重置节点、拓扑邻接、计时结果、累计 IO 时间、skip-save 状态和其他单次运行状态，使 reload 行为不受陈旧元数据污染。Disk-cache diagnostic reset 与 worker record、reader snapshot 使用同一个封装 store；任何 `GraphModel` method 都不能直接读、写、复制、交换或 reset 其中的 optional/path/string storage。

## GraphDefinition 与内存适配器

`GraphDefinition` 是完整图文档的一份深度拥有、格式中立 value。它拥有按顺序排列的
`NodeDefinition` vector。每个 `NodeDefinition` 只包含持久 identity、operation
type/subtype、图像边与参数边、静态 `ParameterMap`、output/cache descriptor，以及
`preserved` 标志。它不包含 runtime parameter、computed output、revision、ROI/LUT state、
timing、dirty state 或 cache result。

`InMemoryGraphDocumentAdapter` 是这份脱离运行态的 value 与私有 `Node`/`GraphModel` state
之间唯一的翻译者：

- apply 会在 stage 完整 `GraphModel::NodeMap` 时校验重复 id 和参数边的必需名称，随后精确调用
  一次 `GraphModel::replace_nodes()`；
- capture 会按 node id 升序访问图节点，只把持久字段复制到独立 definition；
- 单节点 materialization/capture 为 ABI 稳定的 `Host::get_node_yaml()` /
  `Host::set_node_yaml()` 操作提供支持，而不会在 `Node` 上恢复 YAML method。

该 adapter 不拥有 graph、file、parser tree、cache 或 thread。Caller 仍负责使用现有
`GraphStateExecutor` 完成串行化。在 replacement 前发生 definition 或 topology failure，会保留
先前的 node map、topology、generation 与 runtime state。

`GraphDocumentReader` 与 `GraphDocumentWriter` 是两个独立的格式中立 contract。完整图 method
只交换 filesystem path 与脱离运行态的 `GraphDefinition` value；节点 method 只交换拥有所有权的
文本与脱离运行态的 `NodeDefinition` value。两个 contract 都不暴露 yaml-cpp、`GraphModel`、
`Node`、cache state 或 provider-library type。

`GraphIOService` 要求非空的共享 reader/writer owner。它只保留 model orchestration：load 让 reader
返回脱离运行态的 definition，再通过 `InMemoryGraphDocumentAdapter` 应用；save 会先 capture
definition，再调用 writer；node-document operation 也经过同一个注入边界。它不会构造 parser、
emitter 或 graph-document stream。

已配置的 `YamlGraphDocumentAdapter` 拥有私有 YAML translator、filesystem read、node-text
conversion、完整 emission，以及直接 open/write/flush/close 行为。`create_embedded_host()` 在 YAML
启用时选择该 adapter，禁用时选择显式 unavailable document adapter，再把同一个共享 owner 作为
两个 contract 通过 `Kernel` 注入；Kernel 与 GraphIO 都没有默认 persistence construction。一个私有
的显式依赖 Host root 支持确定性的 fake 替换，不增加可安装 API。Issue #61 与 #62 建立中立的
document/value 边界。Issue #63 完成 dependency-disabled product profile；empty/in-memory
session 保持可用，显式 document IO 返回 `GraphErrc::Io`。

## 拓扑邻接

`GraphModel` 拥有 `GraphTopologyIndex`，它记录图边的两个方向：

- `incoming_by_node`：某个节点的上游依赖。
- `outgoing_by_node`：某个节点的下游依赖者。

每条 `GraphTopologyEdge` 都保存稳定的源/目标节点 id、边类型（`ImageInput` 或 `ParameterInput`）、源输出名、目标输入/参数身份和输入槽位索引。成功的图加载、清空、节点添加、节点替换、节点删除和输入重连，都会在图状态暴露给遍历、计算、缓存、inspect、CLI 或 interaction 消费者之前刷新或清空该索引。

## 节点身份

每个 `Node` 包含：

| 字段 | 含义 |
| --- | --- |
| `id` | 图内唯一整数 id。 |
| `name` | 人类可读标签。 |
| `type` | 操作族，例如 `image_process`。 |
| `subtype` | 操作子类型，例如 `gaussian_blur`。 |
| `preserved` | 防止某些强制重算路径丢弃该节点。 |

操作查找通过 `OpRegistry` 使用 `type:subtype`。

## 输入

节点输入按数据类型拆分：

| 输入类型 | 结构 | 含义 |
| --- | --- | --- |
| 图像输入 | `ImageInput` | 读取上游类图像 `NodeOutput`。 |
| 参数输入 | `ParameterInput` | 读取上游命名数据输出，并写入运行时参数。 |

## 参数

`NodeDefinition::parameters` 与 `Node::parameters` 都是包含深度拥有静态数据的
`plugin::ParameterMap` value。已配置 YAML adapter 内部的私有 translator 会把 graph
document 一次转换为脱离运行态的 definition；in-memory adapter 随后把该 definition 复制进
Graph state。Definition 与 Graph storage 都不会保留源 YAML tree。Value 使用精确的
`ParameterValue` alternative：`Null`、`Bool`、`Int64`、`Double`、`String`、`Array`
和以 string 为键的 `Object`。

Inspection 通过格式中立的 `format_parameter_value_for_inspection()` helper 渲染这些 value。
Scalar 拼写保持稳定；array 与 object 递归渲染；object key 保持 ordered-map 顺序；string 会被
加引号并转义，整个过程不会构造 YAML node 或 emitter。

`Node::runtime_parameters` 是另一个 `ParameterMap`，在执行时通过复制静态 value 并应用
`parameter_inputs` 重建。连接的命名 output 会替换同名静态 value，期间不发生格式转换。
算子在计算期间应从 `runtime_parameters` 读取有效值。Executor 会在 request-local node snapshot
上填充它；它不会作为可复用 Graph state 提交。

## 输出

`NodeOutput` 包含：

| 字段 | 含义 |
| --- | --- |
| `compatibility_image` | 仅用于 operation ABI v2、codec 与其余 legacy adapter 的入站暂存。正式 commit 前必须清除，且它绝不是 cache、allocation、readiness 或 revision 权威。 |
| `named_values` | 按规范顺序保存的 immutable Value。当前 image port 永久命名为 `image`；有效 entry 是唯一的 image payload、allocation、readiness 与 revision 权威。 |
| `data` | 作为 `plugin::ParameterMap` 保存的命名标量或结构化输出。 |
| `space` | 空间变换、尺度和 ROI 元数据。 |
| `debug` | worker/设备/计时/范围诊断信息。启用的 CPU range inspection 会通过规范 Value layout 遍历 active scalar byte；padding 被排除，opaque device Value 保留 provider diagnostic。 |

算子可以返回图像数据、命名数据，或两者都返回。

持久 `OutputPort::output_parameters` 是可选、深度拥有的 `ParameterValue`。空 optional 表示
文档字段缺失；已包含值的 null 会保留显式出现的 YAML null。因此，嵌套 output configuration
可以在 parser 销毁后继续存在，而不保留 `YAML::Node`。

对于 tiled `image_mixing`，需要 crop/pad 的 secondary input 会被物化为 request-local
`NodeOutput`：named data、spatial/debug provenance 与 plugin-library lifetime 会被复制，而其
image Value 会替换为通过内核 fill/copy 原语生成、并在 normalized output 暴露前完成 seal 的
aligned storage。Resize 与 channel conversion 继续保留为局部 OpenCV algorithm call。
Normalization context 会持有这些临时 output，直到所有同步 tile callback 完成；shape 完全
匹配的 input 继续借用 upstream output。

DI-2 将 `DenseImageOutputPlan` 冻结为唯一 source-private 普通图像输出描述。该 immutable plan
在 Host allocation 前持有 output name、完整 DenseTensor/ImageFacet fact、精确正向 Strided
layout、byte envelope、base alignment 与完整 image Region。一个 `HostOutputBinding` 拥有
aligned allocation 与 private builder lease。Move-only whole grant 或互不重叠的 tile grant
只暴露经过检查的 row span；overlap、range、alignment、overflow、cancellation、exception、
duplicate retirement 或 omitted retirement 都会使 binding 以关闭状态失败。只有所有 grant
成功 retirement 后，binding 才能 seal 并恰好一次发布一个 Ready Value。该 plan 是唯一的
内部 DI-3 mapping source，而不是临时 ABI record。

## 缓存字段

与缓存相关的节点字段：

| 字段 | 状态 | 含义 |
| --- | --- | --- |
| `cached_output_high_precision` | 正式缓存 | 完整质量、可复用输出的 HP 缓存。 |
| `hp_version` | 正式缓存 metadata | 可复用 HP output 的单调 revision。 |
| `hp_region` | 正式缓存 metadata | 该 HP output 中已知有效的规范化逻辑 Region。 |

只有 HP 输出是正式可复用缓存。这意味着只有 HP 输出可以进入后续 HP 计算、已配置 disk-cache
持久化或单独请求的 output operation；cache entry 本身不是长期用户输出权威。RT 输出不存放在
`Node` 上，而是位于 `RealtimeProxyGraph`，后者镜像 node id，并保存低分辨率 proxy output、
HP-space Region、version 和 RT dirty-source generation。

Dirty RT worker task 会先通过 `RealtimeProxyWriteBuffer` stage proxy output，再提交到
`RealtimeProxyGraph`。Dirty HP worker task 会先通过 `HighPrecisionDirtyWriteBuffer`
stage 正式 HP 输出，再提交到 `GraphModel`；RealTimeUpdate 的 HP commit 会被 gate 到成功的
RT proxy commit 之后。

## YAML Schema

图 YAML 根节点是节点对象序列。支持的节点字段：

```yaml
- id: 1
  name: source
  type: image_source
  subtype: path
  preserved: false
  image_inputs:
    - from_node_id: 0
      from_output_name: image
  parameter_inputs:
    - from_node_id: 2
      from_output_name: value
      to_parameter_name: strength
  parameters:
    path: assets/input.png
  outputs:
    - output_id: 0
      output_type: image
      output_parameters:
        color_space: linear
        channels: [red, green, blue]
  caches:
    - cache_type: image
      location: output.png
```

`id` 是必需字段。其他字段使用已配置 YAML adapter translator 的既有默认值。
`parameter_inputs` 要求 `from_output_name` 和 `to_parameter_name` 非空。
`output_parameters` 可以缺失、显式为 null，或者是任意可表示的递归 `ParameterValue`。

## 空间元数据

`SpatialContext` 携带 ROI 传播和 inspect 使用的变换与 ROI 元数据：

| 字段 | 含义 |
| --- | --- |
| `transform_matrix` | 全局变换矩阵。 |
| `inverse_matrix` | 全局逆变换。 |
| `local_inverse_matrix` | 用于上游 ROI 投影的局部逆变换。 |
| `absolute_roi` | 输出范围或有效区域。 |
| `global_scale_x`, `global_scale_y` | 尺度元数据。 |

`SpatialDependencyMap` 是用于数据依赖空间传播的可选节点本地 LUT。

## 边界与原理

- `GraphDefinition` 是脱离运行态的私有 document value；`GraphModel` 与 `Node` 是私有
  backend runtime state。Public Host caller 与 operation plugin 接收复制的公共 value，而不是
  model reference。
- 结构变更必须经过 model helper，使节点存储、两个方向的邻接、topology generation 与缓存的
  planning state 作为一份一致图状态变为可见。
- Scheduler 只接收 ready-task metadata，绝不拥有节点存储、参数、输出值、拓扑或缓存权威。
- `YAML::Node` 只保留在用于 graph document、共享 value translation 与已配置 cache metadata 的
  私有 YAML adapter 内。Runtime、graph、compute、inspection 或 cache contract 不再声明它，
  `GraphDefinition`、持久 `Node` 字段与 `OutputPort` 也不拥有它。静态/有效参数、output-port
  configuration 与 operation 命名 output 都是 `ParameterValue` tree。逻辑 dirty work 与
  cache validity 使用规范化 `RegionSet`；当前 image extent、physical tile、Host/IPC v2
  inspection 与 operation ABI v2 使用 checked derived `PixelSize` 和 `PixelRect` value。
  只有 OpenCV provider 或算法实现在 matrix slice 或 library call
  确实需要时，才会创建 OpenCV geometry。

### 当前持久化 identity 与完成边界

当前 Graph 文档只序列化用户编写的 node topology/configuration。它排除
`NodeOutput`、正式 HP cache byte、RT proxy state、allocation/value identity、
producer fence、daemon job id、delivery lease 与 output-store record。成功的
`GraphIOService::save()` 会 capture detached definition，并完成已配置 YAML adapter 的
直接 open/write/flush/close 序列；文档没有持久 version，调用也不返回
atomic-replacement 或 durability receipt。

已配置 disk-cache location 仍是 backend cache identity，不是 Graph document id 或
user-output commit id。`ImageArtifactCodec` 与 `CacheMetadataCodec` 转换 image/metadata
表示，但不拥有 transaction、retry、visibility 或 durability policy。私有 IPC
`OutputStore` 保留独立进程级 delivery record 与 artifact identity；这些 value 都不是
`GraphModel`、`NodeOutput` 或 Graph 文档字段。

因此，`ReadyFence::Ready`、正式 HP publication、disk-cache save、Graph 文档保存、
daemon result availability 与受保护 artifact publication 是不同的当前事实。已接受的目标
权威与完成语义分类记录在
[ADR 0009](../../adr/zh/0009-compute-io-durability-and-completion-semantics.zh.md)；
它们不是新增的当前字段。

### 已实现的 V-3 ownership 至 V-15 extension surface

[ADR 0008](../../adr/zh/0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md)
接受完整的通用 Value 替换。V-2 引入了有界 CPU DenseTensor 子集；V-3 现已接通其
physical ownership 与正式 HP cache identity：

- installed `DenseTensorDescriptor` 把 concrete shape、`ElementSemantics`、
  `StorageEncoding` 与可选 quantization 分开；
- installed 普通 `ImageFacet` 显式指定彼此不同的 x/y axis 与可选 channel axis，
  要求有符号半开 data window，并可保留独立 display window、稳定 channel/group
  schema、声明 sample-domain facet 与 color facet；
- `BufferHandle` 是同一显式 storage binding 上受检、不可变、非空的 range；它不暴露 raw
  或 native pointer，并创建保留 identity 的 checked subrange；CPU builder 拥有 host byte，
  而 source-private device publication 可以保留 opaque native owner，并独立记录 host
  visibility；
- `ValueBuilder` 拥有唯一 move-only `WriteLease`，live lease 存在时拒绝 seal，并以全新
  `ValueRevisionId` 发布 immutable byte；
- vector 便捷 constructor 仍会在 seal 前 deep-copy lvalue/rvalue 的
  descriptor/layout/payload allocation；
- `StridedLayout::byte_offset` 锚定 logical coordinate zero；sealed handle 上的 immutable
  Value 可以使用受界限约束的正、零或负 stride；
- `DenseTensorView` 与 `ImageView` 同时保留 Value 和 `ReadLease`，使用 copy-like move
  语义，并只在该 lease 内暴露 pointer；
- `image_process:invert_dense` 对这些 view 执行 descriptor-only inference 与
  stride-aware unsigned-8 execution；它会复用已有 sealed input Value，并发布完全相同的
  sealed result revision。

私有 `NodeOutput::named_values` 是唯一正式 Value 权威；`image` entry 取代过去的
image-buffer/value pair。Producer-pending Value 只能存在于 request-local 临时 output：
`TaskSubmissionPlan` 会让其 Run 保持未 settlement，并在 terminal Ready 后释放 dependant；
Failed、ProducerCancelled 或 stale-typed completion 不会释放任何 dependant。普通 HP commit、
sequential HP compute、connected-preflight shadow cache、dirty HP commit 与 disk decode 都会在
正式发布前拒绝或规范化 compatibility staging。immutable cache copy 保留 allocation 与
revision；dirty/tiled execution 创建一个全新 Host binding，并在所有选中的 executable grant
retirement 后只 seal 一次；replacement 与 disk decode 生成新 identity。allocation/revision
token 只在进程内有效，永不进入 task-graph key、cache path、graph/YAML document 或 artifact
byte。
Shared operation runtime 是 static Host 与每个 Value-using DSO 共用的唯一进程级 minting
authority。

V-4 安装了 `RegionDomainKey`、`ImageRect`、rank-general `TensorSlice`、`RegionAtom`、
immutable normalized `RegionSet`、bounded algebra、typed operation outcome 与 containment。
`Node::hp_region` 是随唯一正式 HP cache authority 一起发布的 validity metadata。Dirty
source history、per-node state、monolithic work 与 edge mapping 都保留 Region；image-only
tile rectangle 从其 source Region 派生并与其并存。Core dense invert path 执行精确
ImageRect 或 TensorSlice selection；RT 拒绝 TensorSlice，operation ABI v2 保持不变。当只有
一个 compatible atom 变化且 overlap 从其一侧移除区间时，精确 one-clause difference 会保留
其他所有相等的 constrained-domain atom；会切分 atom 或同时改变多个 domain 的差集仍返回
类型化 `TooComplex`。

Issue #129 / DI-1 现已把内建普通 DenseImage 元数据具体化。必需的有符号半开
`ImageBounds` data window 是不可变逻辑像素域；其 x/y 跨度必须与显式 axis 上的
descriptor shape 精确一致。负原点与非零原点合法。可选 display window 是呈现
元数据，而动态 dirty/dependency/execution/HP validity 仍属于 `RegionSet`。
`Value::image_bounds()` 在 Pending、Failed 与 ProducerCancelled 状态下暴露 data-window
元数据且不削弱只允许 Ready 的载荷 lease；`ImageView` 分别暴露零基存储索引与有符号
逻辑坐标访问。

可选有界 `ChannelSchema` 使用稳定非零 `ChannelId` 与 `ChannelGroupId`；诊断名称
不选择角色，也不进入语义相等性/digest。版本 1 `SampleEncoding`/
`SampleDomainFacet` 声明 normalized、legal 或 code-value 区间及稳定 ID 逐通道覆盖。
版本 1 `ColorFacet` 把有效 channel group 绑定到显式 transfer function 与 primaries。
存储可表示范围仍只属于 element semantics 与 storage encoding；quantization、声明
sample meaning 与 color 保持独立。观测 min/max/histogram query、result 与完整
revision/content/Region/selector/algorithm cache key 是独立派生值，永远不成为 Value
或 descriptor/content identity。

V-6 为每个 Value 附加 installed、copyable `ReadyFence` observer。同步 publication 初始即为
Ready。Source-private pending producer 保留唯一 mutable CPU allocation capability，在发布
Ready、Failed 或 ProducerCancelled 前撤销它，并允许 pending 状态继续检查
descriptor/layout/size/identity metadata，同时拒绝 payload access。Source-private physical
`ValueTransferTask` 会分配独立 pending CPU destination，并且只在 source ready 后通过
executor-queued work 复制已验证的 envelope。

V-8 安装 `DeviceBackend`、checked `DeviceId`、`MemoryDomain`、`StorageBinding`、
producer identity 与穷尽的 `AccessPlan` outcome。Planning 保持非阻塞，只记录 source fact、
精确 target capability、visibility obligation、lease kind 与 transfer byte，不触碰 payload。
Host access 仍同时要求 producer Ready 与 host-visible binding；否则会抛错，不会隐式等待、
map、import、transfer 或 readback。显式 CPU/Metal transfer 会发布独立 binding，同时保留同一
逻辑 `ValueRevisionId`。进程级 `ResidencyManager` 只索引精确 Ready replica，并以完整
completion identity 加 current supersession generation 原子门控 destination readiness。
一个可失败的 prepublication 步骤会创建 lineage row，但不会指派 managed current identity；
accepted coordinator publication 随后会在暴露 currentness 前指派精确 generation，包括
coordinate 授权的数值下降。之后的 stale Run observation 或 transfer admission 不能替换该
exact managed identity；standalone lineage 另行保持 numeric-maximum ordering。已结算
replica 可以比 producing Run 活得更久，但 manager 默认
最多保留 64 个 entry，从而限制强 native/provider ownership；publication pressure 会释放
revision 最低的 entry。Managed-current 指派本身不会清除 residency，而且这个 entry 数量不是
device-byte 或 scratch admission。在精确 Graph close 排空全部 Run 与 pending native
completion 后，manager 会退役该不复用 `GraphInstanceId` 的全部 generation row。Close tail
还会在本次退役前 join compute-request lane：prepared candidate 会在执行其可失败 lineage
pretracking 之前先拥有一个 reserved lane ticket，因此之后不再有 caller 能重建零 generation
row。仍有 transfer pending 时调用退役属于 invariant failure。这项 Graph-scoped maintenance
不会清除已结算的 resident replica。

V-9 在不改变逻辑 Value identity 或 public binding fact 的前提下新增 byte authority。
`ResourceLedger` 为每个已配置非 CPU `DeviceId` 拥有隔离的 memory/scratch account。Native
plan 使用 backend size/alignment fact，actual allocation 使用 `allocatedSize`。Persistent
device `Value` 的 type-erased external owner 会把唯一 memory lease 与 native allocation
共同保留，因此 Value 副本与 residency 会保留而不是复制该 authority。Scratch 不进入 Value，
而是随精确 asynchronous completion owner 延续。完成后的 HostPinned readback 在 scratch lease
结束后继续保留其 shared Metal buffer，并将其归类为 CPU-owned output storage。

V-12 针对最容易暴露 image-only 假设的维度验证这套已安装模型。dependency-neutral 矩阵覆盖
带 padding image-faceted Value 的 1/3/4/8/16 通道与 FP32/FP64、rank-one 至 rank-five
FP32/FP64 latent Value、精确 ImageRect/TensorSlice merge，以及有界 negative/zero-stride
不可变 view。Rank-one fixture 的唯一 stride 大于 element width，并具有精确 padded storage
span；独立 byte oracle 会验证 active element 与 padding sentinel。CPU-copy 与注入式
external-device preparation 会复用 builder 的正向、零 offset、精确 envelope、non-overlap
校验权威。因此 negative 或 zero stride 会在 external owner、destination identity 或 Pending
fence 逸出前失败，而通用 immutable publisher 仍保留 signed-view 职责。受支持 transfer 会
保留完整正向 producer envelope、descriptor、facet、layout 与逻辑 revision，同时生成不同
allocation 并暴露 Pending-to-Ready binding 事实。已准入的 `ComputeIoExecutor` task 会在
显式 task/byte budget 下保留并观察同一个不可变 Value 的事实与字节；该观察不会创建
cache、artifact 或 persistence identity。

V-13 安装了一条有界 packed/quantized execution 垂直路径，且不会重新解释 byte-addressed
模型。`StorageEncodingKind::Fp4E2M1` 会把 four-bit E2M1 与 floating-point semantics 分开
标识；可选 `QuantizationSchema` 则拥有 rank-matched positive block shape，以及每个 row-major
logical block 对应的一项 finite positive scale。Shape 必须能被完整 block 整除。Version-1
`BlockedLayout` 会独立记录相同 block shape、nibble-aligned block bit stride、absolute bit
offset，以及显式 least- 或 most-significant-first nibble order。Publication 会证明精确 byte
bounds 与互不重叠的完整 block span。`Value` 只保留一个带 tag 的 Strided 或 Blocked layout；
`dense_tensor_element_bytes()` 与 `DenseTensorView` 会拒绝 packed storage，
`PackedDenseTensorView` 则提供 checked encoded-code 与 scale-dequantized access，不会伪造
element byte pointer。

已安装 packed execution operation 只接受 domain 匹配、full-rank、nonempty，且每个 endpoint
都与 quantization block 对齐的 TensorSlice。它会把 FP4 code 与所选 scale 直接复制到 fresh
CPU Value，保留 nibble order 与 bit offset，并生成 canonical contiguous block bit stride，
不会 dequantize 或 requantize。显式 CPU 与注入式 external-device transfer 会在不同 binding
中保留完整 descriptor、quantization、Blocked layout、byte envelope、unused nibble bit、
readiness transition 与逻辑 revision。正式 HP memory cache copy 会保留该 immutable Value 与
精确 TensorSlice validity。当前 image-only disk cache 则会在 compute-I/O admission、filesystem
mutation 或 codec invocation 前，以 `GraphError{InvalidParameter}` 拒绝 packed、quantized 或
latent 正式 Value；这不代表存在通用 artifact format 或持久 digest。

V-14 在 DenseTensor 旁新增显式 `ProviderDefined` representation。
`DataDescriptorEnvelope` 保留精确一个版本化 Schema record 与有界、有序 Facet record；
`ProviderDefinedLayout` 保留一个版本化 Layout record，以及 checked
`{buffer_index, logical_role, offset, length}` envelope。每个 extension record 都拥有自己的
unknown payload byte。Provider-defined `Value` 保留多个 sealed、host-readable
`BufferHandle`、一个不可变的精确 provider generation，以及一个新的进程内 revision。通用
cross-reference 与 checked-end validation 会先于 provider validation 和 publication identity
minting。DenseTensor-only accessor 会拒绝该 representation；indexed `ProviderReadLease` 会同时
保留所选 buffer 与 interpretation generation。

`DataDefinitionRegistry` 是一个注入式、非 singleton authority；它在一个 publication lock 下
拥有一个 generation source、一个 provider table，以及彼此分离的 typed Schema、Facet 和
Layout map。Candidate load 会在一次 atomic publication 前 stage 并验证完整 exact-size v3
definition bundle；typed-key conflict 或 malformed record 会保留所有可见旧状态。Replacement
发布新的完整 generation。Unload 只移除新 lookup visibility：旧 Value、read、callback 与
provider-created owner 会继续保留 retiring generation，直到 final provider destruction，之后
才释放 candidate module lease。持有 registry lock 时不会运行 callback。

本切片实现的 v3 provider ABI 只包含 dependency-neutral definition suite。其 self-contained
C11/C++17 header 冻结精确 record size、offset、alignment、calling convention、status、两个
exported handshake，以及 mandatory validation、纯 property、纯 Region、纯 DataSpec、
canonical-content、owner 和 destroy callback。纯 callback 收到 descriptor/Layout/buffer
metadata，但所有 payload pointer 都会被清空；V-14 中只有 validation 与 canonical-content
traversal 这两个 semantic callback 会收到 payload。Access、mapping、transfer、conversion、
inference、execution、native-device 与 operation ABI v2 authority 均不存在。

借用的 ABI byte view 只用于输入。每个 callback 都会收到一个 Host 拥有的 output sink；
diagnostic 与 BYTES-property record 声明 scalar length，provider 会在 callback-local 源 storage
仍存活时同步复制完整字段。每次 invocation 私有的 Host state 会执行精确 channel 使用规则、
4 KiB/64 KiB bound 与 sticky failure，thread 或 generation 之间不共享 storage。对于 Exact
非空 TensorSlice result，Host 会独立检查 `selected_site_count` 是否等于每个半开轴长度的
checked product；overflow 或 mismatch 会变成 site 为零的 InvalidDescriptor。

有界 V-14 `DataSpec` 把 Schema identity/version 与 logical-site range 求值为 Subset、Disjoint、
PartialOverlapWithRuntimeGuard 或 CannotEvaluate。Property 与 Region call 保留其 typed
unavailable 或 uncertain outcome，包括 Exact TensorSlice count 校验周围的 Empty/Unsupported
Region state。Descriptor、storage-layout 与 provider-selected logical content 分别使用带独立
tag 的 SHA-256 canonical traversal；只要 provider 发出相同 logical byte stream，物理 buffer
order、offset 与 padding 就不会进入 ContentDigest。为了在不 staging 该 stream 的情况下保留
冻结的 length-prefixed field，Host 会先进行一次 checked `uint64_t` measurement traversal，
再在同一个不可变 Value view 与 payload read lease 下重复调用同一 active generation，同时把
byte 增量送入 SHA-256。两次 invocation 使用独立的 callback-local diagnostic state。Chunk
边界不影响结果；畸形 pointer/count 对、overflow、sticky sink failure 与 measured/hash count
漂移均成为带类型 provider failure。不存在 payload-proportional staging 或任意 64 MiB content
上限。版本化 artifact envelope 能在没有 provider 时保留 Schema/Facet/Layout unknown byte 与
三个可选 digest identity，但它不是 graph document、manifest/chunk store、filesystem codec 或
cache-policy integration。

同一个已安装的 `compute_content_digest(Value)` 入口为内建 DenseTensor value 提供冻结的
canonical-v1 stream identity，且不会调用 provider callback。内建 Schema 结构版本 2
编码 rank、shape、element semantics、storage encoding kind/width 以及可选 quantization
block shape 和 binary32 scale bit。Image 结构版本 2 编码 axes、有符号 data/display
windows、稳定 channel 顺序、group IDs 与成员；独立 Sample Domain 与 Color Facet 使用
结构版本 1。诊断名称与观测统计均不存在。Content traversal 按 row-major logical
coordinate 执行：whole-byte
scalar 以 little-endian 发出，blocked FP4 则为每个 logical element 发出一个 low-nibble code
byte。Stride、byte/bit offset、padding、block placement、nibble order、allocation/binding
identity、device identity、readiness metadata 与 Value revision 均不进入 logical content
identity；descriptor-bound quantization metadata 会进入 descriptor digest。Non-Ready 或不可读
payload 返回 `PayloadUnavailable`；malformed 或 unsupported retained state 返回
`InvalidDescriptor`；allocation failure 仍以 `std::bad_alloc` 传播。这些规则让经过 repack、
但逻辑相等的 DenseTensor output 可以用带类型的 `Sha256CanonicalV1` `ContentDigest` 比较，
而不是比较 raw storage byte。

V-15 为这套未改变的 v3 definition suite 提供首个可选具体 generation。仓库自有 OpenEXR
provider 会发布一个 `VariableSampleField` Schema、`ImageFacet`、`DeepSampleFacet` 与一个
multi-buffer Layout。其版本化 descriptor payload 会保留有符号半开 data/display window，以及
从诊断用途的文件 channel 名到永久 channel identity、semantic-role identity 与 Layout buffer
role 的显式 mapping；名称不携带任何推断出的语义。其 canonical Value 保存一个 little-endian
`uint32` sample-count buffer、一个经过检查的 little-endian `uint64` prefix-offset buffer，以及
每个 unit-sampled channel 各自一条按 identity 排序的 FP32 sample stream。所有 stream 都必须
与同一个 declared sample count 一致。由于未改变的 V-14 binding contract 要求每个已发布
semantic buffer envelope 都具有非零长度，全零 deep image 会在 descriptor/Layout payload 中
保留 channel mapping，但省略零长度 channel envelope 与 BufferHandle；其非空 count/offset buffer
仍构成完整 canonical content traversal。

Source-private OpenEXR adapter 通过注入的 `DataDefinitionRegistry` 与
`Value::from_provider_defined` 解码；因此它会复用 V-14 的 cross-reference validation、
generation retention、indexed read lease、Region/DataSpec/property 与三类通用 digest。编码会
检查该通用 Value，而不会定义第二套 deep-image object model。首个 format 严格限于完整的
single-part deep scanline；deep-tiled、multipart、shallow 或 mixed-part 文件、缺失或畸形的
显式 mapping、非 FP32 channel 与非 unit sampling 都会以 Host 自有 typed error 失败。

更多 packed encoding 或 quantization formula、需要 requantize 的未对齐 slice、通用
Map/Import provider、其余 provider ABI suite、通用 graph/cache persistence 与通用命名
immutable Value output 仍属于后续 no-shim slice。V-14 不新增 public resource declaration、
通用 heap suballocation、device-queue budget 或 manifest/chunk。V-15 不新增 deep-tiled 或
multipart 支持、provider-defined Value 的 graph/compute execution path、通用 graph/cache
persistence 或 public OpenEXR type。`PHOTOSPIDER_BUILD_OPENEXR_DEEP_PROVIDER` 默认为 OFF；
在该 profile 下，OpenEXR header、link、symbol、package lookup 与 transitive dependency 均不会
进入 dependency-neutral product。`ParameterMap` 仍用于 configuration 与当前命名 scalar-result
storage。

把图 identity 与 topology 保存在同一个 model 中，可以让 traversal、compute、inspection 与
mutation 观察同一个 generation。Issue #62 在不让已配置 product dependency 变为 optional 的
前提下完成 runtime/cache YAML value 边界。剩余 configured-product 与 provider-library
dependency 工作由
[ADR 0002](../../adr/zh/0002-external-libraries-are-kernel-adapters.zh.md)和精确的
[依赖中立内核目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核)约束；这两份文档都不会
改变上文描述的当前字段。

## 实现与验证入口

- `include/photospider/data/value.hpp`
- `include/photospider/data/extension.hpp`
- `include/photospider/data/image_view.hpp`
- `include/photospider/data/packed_dense_tensor_view.hpp`
- `include/photospider/data/region.hpp`
- `include/photospider/core/device.hpp`
- `include/photospider/memory/access_plan.hpp`
- `include/photospider/memory/blocked_layout.hpp`
- `include/photospider/memory/buffer_handle.hpp`
- `include/photospider/memory/ready_fence.hpp`
- `include/photospider/memory/strided_layout.hpp`
- `include/photospider/plugin/data_definition_registry.hpp`
- `include/photospider/plugin/data_provider_api.h`
- `src/lib/graph/graph_model.*`
- `src/lib/graph/node.hpp`
- `src/lib/graph/graph_definition.hpp`
- `src/lib/graph/graph_document_reader.hpp`
- `src/lib/graph/graph_document_writer.hpp`
- `src/lib/graph/in_memory_graph_document_adapter.*`
- `src/lib/adapters/yaml/graph_definition_yaml.*`
- `src/lib/adapters/yaml/yaml_graph_document_adapter.*`
- `src/lib/adapters/yaml/parameter_value_yaml.*`
- `src/lib/adapters/yaml/yaml_cache_metadata_codec.*`
- `src/lib/core/cache_metadata_codec.hpp`
- `src/lib/ipc/output_store.*`
- `src/lib/core/pending_value.hpp`
- `src/lib/core/value.cpp`
- `src/lib/core/dense_tensor_content_digest.*`
- `src/lib/core/extension.cpp`
- `src/lib/core/packed_dense_tensor.cpp`
- `src/lib/core/value_image_adapter.*`
- `src/lib/core/region.*`
- `src/lib/core/region_image_adapter.*`
- `src/lib/core/cpu_dense_image_operation.*`
- `src/lib/core/ops.cpp`
- `src/lib/core/parameter_value_text.*`
- `src/lib/execution/transfer/value_transfer_task.*`
- `src/lib/execution/device/device_completion.*`
- `src/lib/execution/device/residency_manager.*`
- `src/lib/plugin/data_definition_registry.cpp`
- `src/lib/adapters/openexr/openexr_deep_contract.hpp`
- `src/lib/adapters/openexr/openexr_deep_scanline_adapter.*`
- `plugins/data/openexr_deep_scanline_provider.cpp`
- `src/lib/graph/graph_io_service.*`
- `src/lib/core/ps_types.*`
- `src/lib/compute/dirty/tiled_input_normalizer.*`
- `src/lib/compute/request/compute_metrics_recorder.*`
- `tests/unit/test_graph_topology_boundaries.cpp`
- `tests/unit/test_graph_document_adapter.cpp`
- `tests/integration/test_graph_document_injection.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_stride_aware_compute_paths.cpp`
- `tests/unit/test_dense_tensor_content_digest.cpp`
- `tests/integration/test_graph_document_errors.cpp`
- `tests/integration/test_cpu_dense_tensor_image_operation.cpp`
- `tests/integration/test_packed_fp4_dense_tensor.cpp`
- `tests/integration/test_variable_sample_field_extensions.cpp`
- `tests/integration/test_openexr_deep_scanline_provider.cpp`
- `tests/integration/openexr_deep_provider_option_off_smoke.py`
- `tests/unit/test_region_contracts.cpp`
- `tests/integration/test_value_identity_dso.cpp`
