# 结构化 Result 与 tensor 成员

英文权威文档：[Global-Results.md](../Global-Results.md)。

## 模块边界与所有权

`ResultRef` 承载 workflow 的输入和输出，包括图像。每个 Result 拥有一个由 1..16 个有类型 tensor 成员组成的静态目录、已发布覆盖以及存储所需的 owner。`Tensor`、`TensorSpan` 和 `TensorWriter` 是数据层与 SDK 使用的存储载体。执行采用[编译器与执行](Compiler-and-Execution.zh.md)中描述的固定物理程序。

Result 包含结构 schema 与字节。颜色、alpha、transfer 和采样描述由算子显式配置。编译器校验结构连接，调用方提供满足数值前置条件的输入，算子实现声明的数值语义。执行器不扫描样本以认证领域合法性。

```cpp
struct ExecutionBinding final {
  std::string name;
  ResultRef result;
};
struct ExecutionBindings final {
  std::vector<ExecutionBinding> inputs;
};
```

Workflow 输入按精确名称绑定一个拥有型 `ResultRef`。`RunHandle::join()` 成功时在 `ExecutionResult::results` 中返回具名 owner。Call observer 可以独立保留 `CallRecord::values`。这些引用与拥有型读窗口在 Run 和执行上下文结束后继续保留存储。ObjectId 标识一个逻辑 Result，与物理分配身份和样本内容相互独立。

## 数据布局与内存

`SchemaTemplate` 含 1..16 个 Tensor 成员。每个成员在执行前都有固定的正形状。成员形状、成员数量和行数都不依赖运行期数据。规范 schema 编码域为 v6；它覆盖成员和 publication scope，不包含对象 generation 与 descriptor revision。结构身份编码 schema 的 id/version、成员 key、dtype、cell extent、batch/atomic 轴、可选 spatial 轴索引及 publication scopes。`SchemaTemplate` 校验固定 shape 与 scope 声明，不解释领域，也不规定 storage order 或 row pitch。`ResultRef` 捕获实际 coverage、descriptor revision、物理 stride 与 backing owner。

`ResultTensorSpec` 描述一个 typed Tensor 成员。其 cell descriptor 的 rank 为 1..8，各维 extent 为正；`batch_axes` 增加一个各维为正的前缀，样本总 rank 至多为八。`atomic_trailing_axes` 把末尾若干个 cell 轴闭合为一个不可分割的 observation。Schema 校验不要求稠密分配，也不要求全 domain 元素乘积可表示。`ResultTensorLayout` 记录可选的 spatial layout。Spatial layout 在 cell descriptor 内映射 height、width 和可选 channel 轴，与 batch 前缀无关。Spatial tensor 可以有任意合法通道数；内核不会从通道数或 layout 推断 RGB、alpha、颜色模型或数值范围。

`atomic_trailing_axes` 定义 observation 的完整性：样本区域闭包到这些末尾轴。Payload 授权还遵循成员的 tile 几何。空间 tile 完整覆盖每个非 H/W 的 cell 轴，包括 channel 轴，即使 `atomic_trailing_axes = 0` 也如此。显式 batch 轴选择独立坐标。这种几何覆盖不根据 schema id、shape 或 spatial 标志推断 RGBA 或 alpha 语义。逻辑元组完整性与物理字节顺序无关：在默认 tiled 存储中，一个像素的各通道在逻辑 tile 内是完整的，但位于该 tile 的不同 plane 中。[ADR 0022](../../adr/zh/0022-freeze-operation-c-abi-v3.zh.md) D9 中的 canonical image 目标使用显式逻辑轴角色，不强制 HWC。

```cpp
// photospider/data/result.hpp 的部分公开方法声明。
class ResultBuilder {
 public:
  static Result<ResultBuilder> start(ResourceBudget budget,
                                     const SchemaTemplate& schema,
                                     std::string_view scope,
                                     ResultGrowthLimits limits = {},
                                     std::uint64_t tile_extent = 128);
};
class ResultRef {
 public:
  bool matches_scope(std::string_view scope) const noexcept;
};
```

`ResultBuilder::start` 接受长度为 1..4096 字节的 scope，将其字节复制到 Root 计费的存储，计入复制工作量，并分配新的 ObjectId。`ResultRef::matches_scope` 把已存储的字节与传入字符串做精确比较。执行 preflight 另行根据 Plan 检查 schema、捕获的 coverage、物理布局与存储所有权。Scope 字符串本身不足以证明 Result 满足绑定要求。`SchemaTemplate::publication_scopes` 属于静态 schema 身份；运行时 call 导出遵循有序退休契约。

`ResultRef::capture()` 捕获某个 revision 的已发布覆盖。Producer 后续发布更多样本时，早期 capture 不会获得更多访问范围。`ResultRef::read_tensor()` 要求匹配的 descriptor，仅读取已有覆盖的样本。`ResultDescriptor` 为每个成员保存一个覆盖 `Footprint`。获取的读窗口拥有来源 schema 与 backing，因此 producer 和执行上下文销毁后窗口仍有效。

### 显式 Tensor view 与发布所有权

```cpp
// 公共 Result view 记录摘录；完整声明见 photospider/data/result.hpp。
struct ResultViewAxis {
  std::int32_t output_axis = -1;
  std::uint64_t source_origin = 0;
  std::int64_t step = 1;
  std::uint64_t output_origin = 0;
};
struct ResultTensorViewTransform {
  std::vector<ResultViewAxis> source_axes;
  bool reshape = false;
};
struct ResultTensorViewPiece {
  const ResultTensorReadWindow* source = nullptr;
  Region region;  // 目标坐标。
  ResultTensorViewTransform transform;
};
```

`ResultBuilder::publish_tensor_view(slot, region, source, transform)` 接收借用的、已获授权的 `ResultTensorReadWindow` 与显式 `ResultTensorViewTransform`。`reshape=false` 时，`source_axes` 必须为包括 batch 轴在内的每个完整来源样本轴提供一个 `ResultViewAxis`。映射根据目标**绝对坐标** `output` 计算 `source = source_origin + step * (output - output_origin)`，并检查算术边界。`output_axis=-1` 或 `step=0` 时，来源坐标固定为 `source_origin`。坐标包含目标区域的非零 offset：例如 `source_origin=10`、`step=-2`、`output_origin=5`，则目标坐标 5、6 分别读取来源坐标 10、8。恒等、通道置换与广播使用同一显式映射。

`reshape=true` 时 `source_axes` 为空，完整来源窗口与目标的逻辑样本数一致，且物理的有符号/零 stride 块必须能够合并成仿射映射。逻辑合法但物理布局需要复制时，可返回 `ViewUnavailable`。

多来源时，每个 `ResultTensorViewPiece` 提供一个借用窗口、一个目标坐标中的非空区域以及显式 affine transform。`publish_tensor_view(slot, total_region, pieces)` 至少需要一个 piece；各区域互不重叠、位于 `total_region` 内且并集恰好覆盖它。

**Atomic closure 约束整个 `total_region` 和所有 pieces 的并集，不要求每个 piece 单独闭合。** 例如形状 `[1,2]`、最后一轴为 atomic 的目标，允许两片分别填入通道 `[0,1)` 与 `[1,2)`，两片各自都不是完整的双通道 tuple，但联合后完整。每片仍必须分别通过来源授权和仿射映射检查；多片重载不支持 reshape。

C++ vector 重载的 piece 数量由 Root 的 work 与 metadata 准入约束。piece vector 为空、`source` 为 NULL、piece 中 `reshape=true`，或某片区域超出 `total_region`、与其他片重叠、未覆盖 `total_region` 的一部分时，返回 sticky `TypeMismatch`。

Builder 先校验完整逻辑分区、dtype、来源授权和逐片映射，然后准备物理 spans。之后统一检查所有来源 Result owner（允许来自不同 Roots）的 ownership cycle，并只调用一次 `publish_tensor_spans`。成功时一次认证覆盖并推进 revision，借用窗口释放后仍保留来源 backing。中间 piece 不会单独发布。

View 路径不复制 payload。目标 Root 计入 metadata、几何/work 和 publication；source-backed 字节仍在各来源 Root 计费。直接提供不带 Result 来源 owner 的外部 storage 则通过独立的 `reference(storage)` 准入。

即使某片映射已报告 `ViewUnavailable`，cycle 检查仍会执行；来源链回到目标 Result 时返回 sticky `TypeMismatch`，不提供复制重试。逻辑授权、覆盖重叠、owner cycle、资源失败与取消均不能作为自动复制回退；仅物理不可表示但逻辑合法的映射返回非 sticky 的 `InvalidArgument` 与 `FailureReason::ViewUnavailable`，保留既有 coverage/revision 和早期 capture，算子才可以选择显式物化。其他错误为 sticky；失败或取消的事务不暴露部分组装的视图。

## 执行、发布与生命周期

`ExecutionContext::prepare(plan)` 链接不可变 kernel，并保留模块、资源和设备 owner。`submit(prepared, bindings)` 捕获输入 Result，在执行 task 前校验名称、schema、覆盖、内存域和物理布局。`PlanningOptions::input_domains` 固定输入存储域。Device 绑定必须属于准备时的设备实例。

Plan 声明每个 task 的读取与写入。直接 tile 绑定要求所声明 tile 具有兼容的分配。布局适配和传输由显式 COPY task 完成；DenseHost 输入 lowering 生成 CPU COPY task 来导入稠密 host 输入。COPY 输出使用独立分配。因此 COPY 可以保留每个数值位，同时产生不同的 Result 和 backing owner。

每次 block activation 中，执行器在 issue 前准入输出存储和每个 lane 的 scratch。Kernel 借用固定输入 view、独占输出 view、不可变参数字节和 scratch，直到终态完成。同步 CPU 返回结束借用。已接受 GPU submission 仅在 completion callback 确认所有访问结束且写入可见后结束借用。Task 仍可写 backing 时，输出封装保持私有。

依赖决定 task 是否就绪。CallGroup 按 IR call 顺序退休，`on_call_retired` 在独立串行 lane 上交付记录。成功的 call 在其 task 完成后导出编译时选择的结果；失败或被省略的 call 不导出值。`join()` 等待已接受 task 和 observer 交付全部结束。失败截断、副作用、CFG 边界及取消顺序见[编译器与执行](Compiler-and-Execution.zh.md)。

Tile 存储为声明的消费方、控制流传递和保留的导出保持存活。CFG alias 保留逻辑输入关联，新产出的 Result 获得新 ObjectId。部分输出投影是独立的 Result 对象。Kernel 输入 ObjectId 跨 tile 与成员标识逻辑 Result，不表示分配地址或内容哈希。

### CPU 与 Device 存储

Host 与 Shared 存储支持 CPU 读窗口。Device 存储使用不透明的原生分配，执行结束后仍由 Result 拥有。通过 `read_tensor` 读取 Device 样本，或请求与 Device 覆盖相交的 CPU 窗口，会返回 `BackendUnavailable`。同一成员中由 CPU 存储支持的区域仍可读取。要在 CPU 上读取 Device 输出，需要编译到 Host 或 Shared 存储的 COPY。

COPY task 支持 Host、Shared 与 Device 端点。GPU COPY 的 Host 端点使用已准入的 Shared staging；host gather 先于提交，host download 在终态通知前完成。Metal 与 Vulkan 完成路径保留全部借用存储直到原生工作结束。声明的布局支持有符号和零 stride，包括用少于逻辑样本数量的字节承载 broadcast 输入。

### Result builder

`ResultBuilder` 拥有发布。Producer 通过 `publish_tensor`、`publish_tensor_view` 或 `publish_tensor_kernel` 写入 tensor 区域。Tensor 区域依据成员的逻辑样本 domain 检查。重叠、越界访问或资源准入失败会以错误使 producer 退役。`ResultBuilder::start()` 接收显式 tile extent；非法的 spatial tile extent 在分配前返回 `InvalidArgument`。

发布把物理数组与 backing owner 保存在源自 Result 的存储 owner 中，而发布状态拥有 Root、资源租约与 schema。销毁时先退役数组 backing，再由发布状态释放其 Root 租约。存储 owner 索引避免每次发布都重新扫描此前已发布的 backing；当成员分区允许时，`acquire_tensor` 通过区间查找定位分块，否则使用有界扫描。`acquire_tensor` 在持有发布互斥锁时复制不可变 backing 句柄，然后先解锁再构造拥有型读窗口。`publish_tensor_kernel` 在互斥锁之外准备回调工作，然后重新获取锁，在提交前校验 owner、revision 与 sticky 失败。读窗口保留其 Result 与 backing，只允许读取捕获的 coverage 和所请求矩形内的样本。

`publish_tensor_kernel` 为被请求区域出借可写窗口。Builder 在成员 tile 与 batch 边界处划分请求，并为 spatial 成员提供 tile 内的 planar 字节。它不预留全 domain backing，因此巨大逻辑 domain 上的稀疏请求按实际可写 tile 计费。回调必须使用每个窗口的 `spec()`、`region()`、`sample_axis()` 以及 `row_run()` 或 `rectangle_run()` 返回的 stride。回调不得假定跨窗口的 planar backing 或连续行。

```cpp
Status ResultBuilder::publish_tensor_kernel(
    std::uint32_t slot, const Footprint& samples,
    const std::function<
        Status(const ResourceVector<ResultTensorWriteWindow>&)>& write,
    const CancellationToken& cancellation = {});
```

Footprint 重载在一个事务中发布一组精确、互不相交的规范 box。Builder 在运行任何 producer 代码之前复制 `samples`，因此修改调用方 footprint 对象的回调不会改变发布集合。Region 重载以单个 box 委托到此路径。Footprint 必须已按 tensor 的 observation 分组闭合（`close_samples` 返回同一集合），且不得与已有覆盖重叠；违反时返回 `TypeMismatch`。每个 box 按成员 tile 和 batch 边界拆分。回调为每个 chunk 接收一个窗口，先按 `boxes()` 顺序，再按各 box 内的逻辑 chunk 顺序排列；所有窗口共享一个私有分配和一次发布事务。空洞不分配存储，也不获得覆盖。空 footprint 跳过回调与 payload 工作。

回调在发布互斥锁之外运行，运行期间拒绝同一 producer 的修改与封存。回调返回后，builder 重新检查 owner、revision 与 sticky 失败，再检查取消。返回的错误、抛出的异常（分配失败为 `ResourceExhausted`，其他为 `OperationFailed`）、取消或 revision 变化（`Stale`）会记录 producer 失败、释放缓冲区且不认证新的覆盖；此前已认证的前缀仍可读取。通过窗口写入的 worker 必须在回调返回前完成，因为窗口与裸指针在那时失效。成功时，builder 在一个新 revision 上同时提交这些 box 与覆盖。

`ResultTensorWriteWindow::borrow_dense(spec, writer)` 无复制地把显式行主序 `TensorWriter` 作为写窗口出借。它先校验成员的 spatial 拓扑，再以 `TypeMismatch` 拒绝 dtype 或完整样本形状不同的 writer。

物理 tensor view 保留其发布字节所来自的每个来源 Result。ICC profile 或 OCIO 配置等不可变算子资源通过独立的 `ResourceSet` 提供。静态准备声明其 `ResourceIdentity`；registry 选择这些资源，并在发布 prepared operation 前调用 `validate_resources`。Prepared kernel 保留所选资源和定义模块直到执行完成。Result 独立保留自己的 schema、backing 和计费所有权。

## 结构契约与诊断记录

`OperationPortConstraint` 描述 schema 身份、成员选择、dtype 与 rank。`infer_operation_outputs` 检查全部输入，包括从运行时读取中排除的输入；同构重复输入组比较完整逻辑 shape 和 dtype。静态准备还通过 `OperationDefinition::validate_resources` 检查声明的不可变资源。数值样本合法性由算子与调用方负责。

```cpp
// 部分公开字段，完整声明见 fixed_invocation_api.h。
struct ps_kernel_status_v8 {
  uint32_t code, reason;
  uint64_t domain_code;
  char message[256];
};
```

固定 kernel 以有界 operational status 返回或完成。Host 把 `domain_code` 作为不透明诊断信息保留。Completion 记录使用预分配存储，协调器把它们转换为拥有型 `Status` 对象。`ExecutionResult::diagnostics` 报告 activation、submission、completion、retirement 计数和活动 task 峰值。

独立的 `DiagnosticAccumulator` API 提供 Root 计费、固定容量的报告合并。`OperationRegistry::register_diagnostic` 注册 codec，`OperationTraits::diagnostics` 绑定身份用于静态解析。`DiagnosticDefinition::validate` 检查报告字节，`merge` 产生完整替换记录。失败保持 accumulator 不变。使用 accumulator 的调用方串行化其修改，返回的字节 view 仅在对应记录保持不变期间有效。固定调用 ABI 8 没有报告提交回调，Run 不暴露累积报告流。

## 限制与失败处理

每个执行上下文拥有托管 `ResourceBudget`。`ExecutionContextConfig::managed_resources` 提供可选上限，`maximum_live_bytes` 限制 payload 容量。`resource_budget()` 返回共享句柄，其租约可以比 context 存活更久。托管容量计入已插桩的分配与预留，不是 RSS 上限。

Plan 的物理上限决定 block 输出与 scratch 预留。实际保留的 Result owner 在 block 退休后继续保留存储计费。Root 计费的 metadata 包括托管 schema 副本、coverage、目录和拥有型容器。只要选中的物理 tile 与 metadata 满足上限，巨大逻辑 shape 就是合法的；逻辑大小本身不要求稠密分配。

除上文允许显式复制重试的 `ViewUnavailable` 情形外，Result 发布错误具有 sticky 语义。失败或取消的发布不暴露新覆盖，先前发布的区域仍可通过捕获的 descriptor 读取。`seal()` 在当前覆盖处关闭 producer，包括稀疏覆盖，并拒绝后续写入。它不检查数值样本，也不填充空洞。

取消停止新工作，并在释放借用 owner 前排空已接受 task。Context 销毁请求取消并等待活动 Run 和 observer。最后一个拥有型 Result 或读窗口释放时，其保留的 backing 随之退役。
