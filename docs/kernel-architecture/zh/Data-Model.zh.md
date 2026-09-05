# 数据模型

## Source 与 compiler value

`WorkflowDocument` 包含 version、bounded node、typed scalar parameter、input edge 与 named
output。它是 caller-owned compiler input，不是 file format 或 storage object。

`SemanticGraphIR`、`OptimizedGraphIR` 与 `ExecutionPlan` 是具有独立 typed digest 的
immutable stage value。它们包含 copied operation trait 与 stable key，不包含 callback、
DSO handle、runtime allocation 或 daemon id。

## Runtime Value

当前 dense `Value` 包含：

- `ValueDescriptor`：`UInt8`、`Int64` 或 `Float64`，以及 rank 1..8 的 nonzero shape；
- 一个 rank-matching logical `Region`；
- `StridedLayout`：byte offset 与每个 axis 的 signed byte stride；
- 最多 64 个 unique versioned `ValueFacet` key/payload record；
- 一个 shared immutable owned byte vector。

`Value::create` 在原子 publication 前检查 rank、shape、Region containment、element
vocabulary、stride count、signed offset/span arithmetic、overflow、element tail、完整
buffer bound、facet key/version、duplicate key 与 bounded facet payload。只有 addressed byte
range 位于 buffer 内时才接受 negative/zero stride。Facet 按 key 排序；副本共享
immutable bytes，且不暴露 writable pointer。

`Region` 使用 descriptor-axis order 的 unsigned offset/extent pair。它是 logical subset，
绝不是 byte range。Interval addition 与 element-count multiplication 都经过 checked
arithmetic。

`Value::as_float64()` 是严格 scalar accessor：除了精确 Float64 descriptor、contiguous
layout 与 storage bound，Value Region 还必须为 rank one 且精确覆盖
`{offset=0, extent=1}`。Empty、partial 与 offset Region 仍是合法的通用 Value
coverage，但通过该 accessor 读取时返回 `TypeMismatch`。

## Result 与 data definition

`ExecutionResult` 是 caller-owned named Value map 加 raw diagnostic。它没有 durable
identity、retention、receipt、serialization 或 recovery contract。

Data-definition registry 从 startup configuration 或 trusted DSO 复制 schema key、element
type 与 maximum rank，然后 freeze。Provider load 只接受 platform loader 前已验证的精确、
非空、1..4096-byte 且不含 embedded NUL 的 path；malformed path 返回
`InvalidArgument`，无法加载的合法 path 返回 `NotFound`。它不构造 Value，也不提供
storage。

## 已接受的 S1 目标，尚未实现

开发方向与 Float32 目标已经接受。[ADR 0016](../../adr/zh/0016-workflow-inputs-and-execution-bindings.zh.md)
已修订图像、普通标量、逐端口需求和 operation ABI v3 的具体方案；契约已经
Accepted。上述当前实现事实不变，未实现新元素或绑定；#256 跟踪决策交付，#257 负责实现。
