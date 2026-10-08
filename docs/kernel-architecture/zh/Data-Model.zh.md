# 数据模型

## 1. 模块职责与所有权

Compiler 持有 workflow 结构和推导 metadata 的不可变描述。Structured operation 通过具名 binding 交换 `ResultRef` owner；Result 可包含 typed tensor slots、packed fields 或两者。`Value` 保留为 typed backing 和本地数据容器，`PlanarImage` 提供图像存储与导入能力。返回的 Result 在 execution context 退休后仍持有其存储、resources 与 accounting leases。

## 2. 核心结构与内存布局

```cpp
struct WorkflowDocument {
  std::uint32_t schema_version = 5;
  std::vector<WorkflowInputDeclaration> inputs;
  std::vector<WorkflowNode> nodes;
  std::vector<WorkflowOutput> outputs;
};

class Region;  // descriptor 轴顺序中的逻辑 offset/extent 区间

class Value {
 public:
  static Result<Value> create(ValueDescriptor, Region, StridedLayout,
                              std::vector<std::uint8_t>,
                              std::vector<ValueFacet> = {},
                              ResourceBindings = {});
};

struct ExecutionBinding {
  std::string name;
  ResultRef result;
};

class PlanarImage {
 public:
  static Result<PlanarImage> create(ValueDescriptor, PlanarImageConfig,
                                    std::vector<ValueFacet> = {},
                                    ResourceBindings = {});
  static Result<PlanarImage> import_value(const Value&, PlanarImageConfig,
                                          const CancellationToken& = {});
};
```

`WorkflowDocument` 是复制后送入 compiler 的 source。Node 带有 operation key、按端口排序的输入引用、typed parameter 和命名输出选择。Input declaration 必须提供固定的 `result_schema`；缺少 schema 时 compiler validation 返回 `InvalidArgument`。Execution binding 提供对应的 owning `ResultRef`，其 schema 和静态 metadata 会与 declaration 比对。Plan 保留 declaration 与选中的 resource identity，不保留调用方 sample 地址。

通用 `Value` 包含 rank 为 1..8 且 extent 非零的 descriptor、逻辑 `Region`、`StridedLayout`、最多 64 个唯一 facet，以及共享的不可变 CPU 可访问存储。Element type 包括 `UInt8`、`Int8`、`UInt16`、`Int16`、`Int64`、`Float32` 和 `Float64`。`Value::create` 在发布前校验 shape 和 coverage、stride 数量与寻址字节范围、element type 以及 facet/resource 一致性。只要全部寻址字节都位于 backing allocation 内，负 stride 和零 stride 均有效。副本共享存储，不暴露可写指针。

`Region` 按 descriptor 轴顺序保存无符号 offset/extent。它描述逻辑样本，不表示字节范围。Region interval 和 element count 使用 checked arithmetic。`Value::as_float64()` 只接受连续的 Float64 scalar，且 Region 必须为 rank one 并精确等于 `{offset=0, extent=1}`；其他合法 coverage 仍可通过通用 accessor 使用。

`BufferAllocator` 在分配前预留容量。只能移动的 mutable buffer 在发布时将租约转移给不可变存储。`Value::from_storage` 和 `view` 可以无复制共享 backing；`bytes()` 返回借用视图，`copy_bytes()` 明确复制到调用方所有的内存。存储可超过 allocator 和 execution context 的寿命。Regional-source callback 同步填充请求的 packed Region；可写目标和 allocator 指针在回调返回时失效。

`PlanarImage` 表示结构化 rank-2 或 rank-3 存储，带有显式图像轴、分量组、facets 和 resources。它的虚拟地址预留、已 backing 页面、metadata 容量和有效样本是不同资源。连续和 tiled 存储都保证每行样本连续。Tile 高度与宽度必须为正的 2 次幂；图像或 ROI 可以在部分 tile 结束。

样本发布后才可读取。已发布样本不可变，存储租约保留到最后一个 image 或 read-window owner 释放。预算耗尽不会驱逐存活 backing。`acquire` 返回保留 owner 的读取窗口；`row_run` 和 `rectangle_run` 只暴露获准范围，并受请求 Region 和物理 tile 边界限制。`read` 复制到调用方所有的 packed 存储。事务写窗口在 commit 时发布获准输出范围，失败时丢弃未发布写入。

## 3. 调度与状态

```text
WorkflowDocument -> declarations + inferred descriptors -> ExecutionPlan
          |                                              |
          +---- 调用方持有的具名 bindings ---------------+
                                                         v
                                                    ExecutionRun
                                           +-------------+-------------+
                                           |                           |
                                  Result producers 与 typed tensor slots
                                           |                           |
                                      命名 Result outputs     dependency evidence
                                           +-------------+-------------+
                                                         v
                                                ExecutionResult
```

`ExecutionBindings` 使用精确名称并为 structured operation 提供 Result owners。Binding 在 callback 前检查 declaration schema 与静态 metadata。Plan 不捕获 runtime address，因此独立的不可变 bindings 可独立或并发执行同一个 current plan。`Value` 与 `PlanarImage` 可作为本地或 backing storage，但不是 structured operation 的替代 port。

Planning 保留两个 closure。Metadata closure 沿 output-reachable operation 的全部静态 input 展开，以保留完整 schema、specialization 与 identity。Executable closure 从命名 output 和 side-effect root 开始，再只沿所选 output 的 `input_indices` 展开；未声明 projection 时展开该 node 的全部 input。Backend placement 与准入只作用于 executable closure。只用于 metadata 的 input 仍接受静态校验并参与 identity，但其 producer 不执行。直接请求 GPU-only output 时，`CpuExact` 仍返回不可用错误。

Workflow input declaration id 与 node id 使用不同命名空间。Document 最多有 4096 个 declaration；每个 declaration 的 id 非零且唯一，name 唯一并由 1 至 128 个可打印 ASCII 字节组成，取值范围为 `0x21` 到 `0x7e`。Compiler stage 按 id 顺序携带 declaration。每个 Result input declaration 都携带完整 schema；缺少 schema 时返回 `InvalidArgument`。Compiler 根据静态 input domain 解析 input-derived extent，不读取 payload samples。

当 Result binding 引用 execution root 之外的 storage 时，runtime 会按 `Referenced` 准入其保留容量。已发布的 view 保留其 source Result 与 backing owner。Result tensor read 受 captured descriptor 与请求授权区域限制；publication 和 window ownership 见[Structured Results and tensor slots](../Global-Results.md)。

`ExecutionResult` 包含具名 Result outputs、dependency evidence 和 diagnostics。每个返回的 Result 持有其存储 leases，直到最后一个 owning reference 释放。

## 4. 算法与校验

Result schema 描述逻辑 sample domain；Compiler 不要求其 dense element product 或 packed byte count 能表示为机器 allocation size。对端口类型为 `RgbaFloat32` 的输出，planner 检查其 packed dense size 可以表示；Result planning 不估算 dense payload bytes。实际 `Value` backing 创建时会校验 allocation size 和寻址字节范围，stride 也必须适配有符号表示。Result 的逻辑 domain 可以远大于物理 backing，例如由小 allocation 支持的 broadcast view。

Result tensor schema 描述逻辑轴和可选空间布局。Tiled physical address 不能用普通仿射 stride 表示。各 operation 声明支持的 Result metadata 与执行行为；空间规划需要时，`PlanningOptions` 提供 physical tile 几何。

Float32 Value 保留 binary32 payload bits。Scalar 或 operation contract 校验实际消费的数值定义域；存储层独立处理 element 表示。

## 5. 限制与非目标

- Result 是内存中的值，没有持久 identity、receipt、serialization 或 recovery contract。
- Data-definition registry 保存来自受信任启动注册或 DSO 的复制 schema metadata；它不分配 Value，也不提供存储。
- Structural image 能力由 Result tensor schema 和 operation contract 声明；不支持的组合会在校验或规划阶段返回带类型的失败。
- `CpuStorage` 暴露 CPU 可访问的不可变字节，也可保留已完成的原生存储。Device handle 保持私有。
