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

## Result 与 data definition

`ExecutionResult` 是 caller-owned named Value map 加 raw diagnostic。它没有 durable
identity、retention、receipt、serialization 或 recovery contract。

Data-definition registry 从 startup configuration 或 trusted DSO 复制 schema key、element
type 与 maximum rank，然后 freeze。它不构造 Value，也不提供 storage。
