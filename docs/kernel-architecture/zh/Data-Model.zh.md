# 数据模型

## 1. 模块职责与所有权

Compiler 持有 graph 结构和推导后 metadata 的不可变描述。每次 execution 接收具名输入 owner，并创建 runtime Value 或 PlanarImage。`ExecutionResult` 持有请求的命名输出和 diagnostics；共享 owner 与资源租约决定返回存储的生命周期。

## 2. 核心结构与内存布局

```cpp
struct WorkflowDocument {
  std::uint32_t schema_version = 3;
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
  Value value;
  std::shared_ptr<const RegionalSource> source;
  std::shared_ptr<const InputSnapshot> snapshot;
  std::shared_ptr<const PlanarImage> image;
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

`WorkflowDocument` 是复制后送入 compiler 的 source。Node 带有 operation key、按端口排序的输入引用、typed parameter 和命名输出选择。Input declaration 描述固定 metadata；Execution binding 提供对应 payload 或 image owner。Plan 保留 declaration metadata，不保留调用方像素地址。

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
                                     generic Value              PlanarImage
                                           |                           |
                                      命名 Value                 命名 image
                                           +-------------+-------------+
                                                         v
                                                ExecutionResult
```

`ExecutionBindings` 使用精确名称。普通通用 execution 的输入选择 `Value`、`RegionalSource` 或 `InputSnapshot` 之一；planar declaration 选择 `image`。Planar execution 中，非 planar declaration 只接受匹配的 `Value`，planar declaration 则要求 `PlanarImage`。Execution 在 callback 前检查 declaration name 与 metadata。Image binding 还要核对 descriptor、facets、结构布局和 plan tile 几何。Plan 不捕获 runtime address，因此独立的不可变 bindings 可独立或并发执行同一个 current plan。

Workflow input declaration id 与 node id 使用不同命名空间。Document 最多有 4096 个 declaration；每个 declaration 的 id 非零且唯一，name 唯一并由 1 至 128 个可打印 ASCII 字节组成，取值范围为 `0x21` 到 `0x7e`。Compiler stage 按 id 顺序携带 declaration。每个 declaration 固定完整 descriptor、whole Region、canonical dense input layout 和精确 facet 集合；planar declaration 使用 `planar_layout`，affine layout 为空。

Planar execution 期间，Run 会 pin 外部 image owner，阻止其发布，并准入稳定的 resident capacity。对同一 owner 的重复引用及同 context result rebinding 共用一条计费记录。普通 read window 仍允许向不相交区域发布数据。页面、tile 和发布几何见[张量存储与区域访问](../../kernel-specs/zh/Tensor-Storage-and-Region-Access.zh.md)。

`ExecutionResult` 可以包含具名通用 Value、具名 PlanarImage、受支持的 structured result 和 diagnostics。Planar image 不会隐式导出 dense Value。每个 result owner 持有其存储租约，直到最后一个 owner 释放。

## 4. 算法与校验

对 dense 通用输入，Compiler 在不分配 payload 的情况下用 checked product 校验 packed byte count。字节数必须大于零，最后一个可寻址字节必须适配 `INT64_MAX`，总大小必须适配 `SIZE_MAX`；row-major stride 也必须适配有符号表示。通用 runtime Value 可使用非 dense stride 和 partial Region，前提是寻址范围位于存储内。

图像输入 declaration 带有 `planar_layout`；由于 tiled address 不能用普通仿射 stride 表示，其 affine layout 为空。Layout 描述 axis、storage mode、row pitch 和 component group。各 operation 独立声明 planar output 支持。整个 DAG 使用 `PlanningOptions` 提供的 tile 几何，Execution 会与图像 binding 核对。

Float32 Value 保留 binary32 payload bits。Scalar 或 operation contract 校验实际消费的数值定义域；存储层独立处理 element 表示。

## 5. 限制与非目标

- Result 是内存中的值，没有持久 identity、receipt、serialization 或 recovery contract。
- Data-definition registry 保存来自受信任启动注册或 DSO 的复制 schema metadata；它不分配 Value，也不提供存储。
- Structural planar callback 能力取决于 operation trait 和具体入口。不支持的组合返回带类型的失败，不进入旧图像代码。
- `CpuStorage` 暴露 CPU 可访问的不可变字节，也可保留已完成的原生存储。Device handle 保持私有。
