# 数据模型

## Source 与 compiler value

`WorkflowDocument` 包含 version、bounded node、typed scalar parameter、input edge 与 named
output。它是 caller-owned compiler input，不是 file format 或 storage object。

`SemanticGraphIR`、`OptimizedGraphIR` 与 `ExecutionPlan` 是具有独立 typed digest 的
immutable stage value。它们包含 copied operation trait 与 stable key，不包含 callback、
DSO handle、runtime allocation 或 daemon id。

## Runtime Value

通用 regional `Value` 包含：

- `ValueDescriptor`：`UInt8`、`Int64`、`Float64` 或 `Float32`，以及 rank 1..8 的 nonzero shape；
- 一个 rank-matching logical `Region`；
- `StridedLayout`：逻辑 origin、byte offset 与每个 axis 的 signed byte stride；
- 最多 64 个 unique versioned `ValueFacet` key/payload record；
- 一个共享只读 CpuStorage 所有者。

`Value::create` 在原子 publication 前检查 rank、shape、Region containment、element
vocabulary、stride count、signed offset/span arithmetic、overflow、element tail、有效 Region 的
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

BufferAllocator 在精确容量的 CPU 分配之前取得租约。MutableBuffer/MutableValue
只能移动，发布消耗 writer，并让只读存储继续持有租约。Value::from_storage 无复制地
验证 origin-relative coverage；view 共享所有者并收窄范围，byte_address 检查逻辑坐标。
bytes() 返回借用 ByteView，copy_bytes() 明确复制到调用方所有的内存。存储与租约可
晚于 allocator 销毁。test_storage 验证部分/反向视图和最后引用释放。
全执行资源准入让分配租约存活到最后一个所有者释放。通用数值区域源使用此存储，
其同步流式 sink 接收仅在回调内有效的借用 ValueView。图像执行采用下述结构存储契约。

## 结构图像存储

Package 0.19 采用唯一图像内存契约，见
[张量存储与区域访问](../../kernel-specs/zh/Tensor-Storage-and-Region-Access.zh.md)。
`PlanarImage` 是 rank-2／rank-3 通用张量的物理 owner，携带显式图像轴、分量组、
facets 和 resources，不是 RGB／Layer 特殊语义载体，也不执行颜色运算。
每张图像预留一段连续虚拟地址。连续平面和采用 DAG 尺寸的分块平面均保持行内
样本连续。分块存储的右边缘补齐到 tile 宽度，底部只保留有效行，每个 tile 起点页对齐。tile 高宽必须分别为正的 2 次幂；
规划和图像创建拒绝非 2 次幂几何。图像及 ROI 尺寸无需是 2 次幂，边缘规则见存储契约。

虚拟预留、页 backing、metadata 容量和有效样本分别管理。算子访问前显式准备并
准入页面；只有成功发布才使精确样本范围有效，同页未产生样本仍不能通过 API 读取。
已发布样本不可变。已产生 backing 和租约保留到最后一个图像／窗口 owner 释放；
超预算不会驱逐存活页或回放 producer。

`PlanarImage::import_value` 是显式交错／strided 导入边界。`acquire` 提供保留
owner 的精确读取窗口及有界 `row_run`；`rectangle_run` 提供带显式字节步长的多行区域，
同时受 ROI 和物理 tile 边界约束，padding 不可访问。FMT-01 使用这些矩形分摊坐标
校验开销并按 tile 顺序复制。`read` 显式把请求区域复制到调用方 packed
存储。宿主准备的事务写窗口只提供获准输出行段，操作成功时提交，失败时回滚未发布
资源。完整预留地址绝不作为可无条件读取的 ByteView；raw 数值解释也不能绕过物理
访问规则。

图像执行在 Run 期间固定外部输入 owner，阻止发布，再对稳定的保留容量准入。
普通读取窗口仍允许不相交发布。共享计费识别重复 owner 和同 context 结果回绑，
避免重复收取同一 backing。

## Result 与 data definition

`ExecutionResult` 拥有具名通用 `values`、planar `images`、已支持的 structured
results 和 raw diagnostics。图像不自动导出 dense Value。结果保留存储租约，但没有
durable identity、receipt、serialization 或 recovery contract。

Data-definition registry 从 startup configuration 或 trusted DSO 复制 schema key、element
type 与 maximum rank，然后 freeze。Provider load 只接受 platform loader 前已验证的精确、
非空、1..4096-byte 且不含 embedded NUL 的 path；malformed path 返回
`InvalidArgument`，无法加载的合法 path 返回 `NotFound`。它不构造 Value，也不提供
storage。

## Workflow input 与绑定快照

Schema 3 使用 `WorkflowInputDeclaration` 和 tagged `WorkflowInput` source：
`WorkflowNodeOutput` 或 `WorkflowInputReference`。Node id 与 declaration id 使用独立
命名空间。最多 4096 个 declaration，nonzero id 和精确的 1..128-byte 可打印 ASCII
name 必须唯一，name 不含空格。各 compiler stage 的 `input_declarations()` 按 id 排序复制。

通用数值 declaration 固定 UInt8/Int64/Float64/Float32 descriptor、whole Region、零 byte offset、
正的 canonical row-major stride 和精确闭合 facet 集合。无需分配 payload 即检查 dense
byte count B：B > 0、B - 1 <= INT64_MAX、B <= SIZE_MAX；每个存储 stride 必须适配 int64。
通用 Value 保留已有 strided/partial-Region 行为。

图像 declaration 使用 `planar_layout` 且仿射 layout 为空。轴、存储模式、行 pitch
和分量组属于 compiler metadata。`OperationOutputTraits.planar_layout` 独立于输入
声明每个受支持的 planar 输出。布局与能力变化进入编译身份，运行时地址不进入。
整个 DAG 的 tile 几何来自 PlanningOptions，并与所有图像绑定核对。

`ExecutionBindings` 使用精确名称。通用输入选择 Value、RegionalSource 或非图像
InputSnapshot；planar 输入只选择 `image`。回调前校验 source metadata 和所需名称；
图像绑定检查 descriptor、facets、结构布局和 tile 几何。Plan 不持有输入像素地址。
独立绑定可重复或并发执行同一 current plan。

CPU planar 回调路径支持至少一个 planar 输入的显式单输出 Whole／Elementwise 算子。
不支持的 traits 与未迁移图像算子明确失败；旧 image／Layer 结构 schema 不能提供
备选图像存储路径。旧 packed 图像 binding、snapshot 和 ValueFragments 路径
不是备选图像实现；非图像 Value 能力保留。图像 demand、streaming、frozen／atom
和 GPU 入口需要各自结构迁移；未支持入口拒绝执行，不调用旧图像代码。

Float32 使用 element code 4，通用 Value 保留全部 IEEE binary32 bit pattern。
标量／算子契约负责实际消费的数值定义域检查。
[图像算子](Image-Operations.zh.md) 和 [ADR 0016](../../adr/0016-workflow-inputs-and-execution-bindings.md)
中的原 typed-image 定义域，不构成 planar 契约下运行旧图像路径的授权。

CpuStorage 表示 CPU 可访问的不可变存储，可持有已完成的 Metal shared buffer。发布后 bytes 可读，原生 owner 与预算 lease 可超过 ExecutionContext 寿命；设备句柄保持私有。
