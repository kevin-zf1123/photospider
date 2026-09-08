# 数据模型

## Source 与 compiler value

`WorkflowDocument` 包含 version、bounded node、typed scalar parameter、input edge 与 named
output。它是 caller-owned compiler input，不是 file format 或 storage object。

`SemanticGraphIR`、`OptimizedGraphIR` 与 `ExecutionPlan` 是具有独立 typed digest 的
immutable stage value。它们包含 copied operation trait 与 stable key，不包含 callback、
DSO handle、runtime allocation 或 daemon id。

## Runtime Value

当前 dense `Value` 包含：

- `ValueDescriptor`：`UInt8`、`Int64`、`Float64` 或 `Float32`，以及 rank 1..8 的 nonzero shape；
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

## Workflow input 与绑定快照

Schema 2 增加 `WorkflowInputDeclaration` 和 tagged `WorkflowInput` source：
`WorkflowNodeOutput` 或 `WorkflowInputReference`。Node id 与 declaration id 使用独立
命名空间。最多 4096 个 declaration，nonzero id 和精确的 1..128-byte 可打印 ASCII
name 必须唯一，name 不含空格。各 compiler stage 的 `input_declarations()` 按 id 排序复制。

Declaration 固定 UInt8/Int64/Float64/Float32 descriptor、whole Region、零 byte offset、
正的 canonical row-major stride 和精确闭合 facet 集合。无需分配 payload 即检查 dense
byte count B：B > 0、B - 1 <= INT64_MAX、B <= SIZE_MAX；每个存储 stride 必须适配 int64。
通用 Value 保留已有 strided/partial-Region 行为。

`ExecutionBindings` 是精确 name 对应的 `ExecutionBinding` Value vector。每个 declaration
包含未使用的声明都必须绑定一次。重复、缺失、多余、非法 name 和无效 Value 返回
InvalidArgument；合法但不匹配的 type/shape/Region/layout/byte-count/facet 返回
TypeMismatch。每次调用复制 name 和 Value metadata，共享 immutable byte ownership。
Plan 不保留 binding 或 payload address。同一 current plan 可重复或并发执行独立快照。

Float32 使用 element code 4，通用 Value 保留全部 IEEE binary32 bit pattern。
图像/标量 port 增加各自的 finite domain 检查，参见
[图像算子](Image-Operations.zh.md) 和 [ADR 0016](../../adr/0016-workflow-inputs-and-execution-bindings.md)。
