# 结构化 Result 与 tensor 成员

英文权威文档：[Global-Results.md](../Global-Results.md)。

## 模块边界与所有权

`ResultRef` 是全部 workflow 数据（包括图像数据）的发布、ownership 和 input/output 路径。Result 是由 1..16 个 typed Tensor 成员组成的静态目录。成员可以声明 spatial layout；这不创建独立的图像 Result 路径。`Tensor`、`TensorSpan` 与 `TensorWriter` 是内核与 SDK 内部的存储载体，不是 workflow 或 operation 的 input/output 形式。

Result 只携带结构、数据以及存储所需的 owner。它没有附着的领域描述、semantic kind 或 facet 列表。内核不会把颜色、alpha、transfer 或采样描述从输入传播到输出，也不会在连线上匹配这类标签。结构相同的两个 Result 可以连接到同一个端口，即使调用方赋予它们不同含义。需要领域解释的算子以显式静态参数或显式的算子领域配置接收该解释，并可通过绑定的校验器检查样本合法性（见[端口校验器与诊断报告](#端口校验器与诊断报告)）。

当前 package 为 0.38.0。WorkflowDocument schema 为 7，OperationTraits version 为 29，Result C operation ABI 为 7，CPU parallel 与 CPU tiles 服务为 version 2，native GPU 服务为 version 1。这些版本号描述彼此独立的契约。C++ operation interface 使用 `start_result`、`ResultContinuation` 和静态 `map_dependencies` mapper。Result C operation table 是唯一的 C operation-plugin 接口；consumer 必须针对已安装 package 重建，因为 C++ API 与精确大小的 C 声明是契约。[ADR 0022](../../adr/zh/0022-freeze-operation-c-abi-v3.zh.md)（Proposed）记录所有 operation 共用的按前缀协商 C ABI；package 0.38.0 尚未实现。

编译器把不可变 schema 复制到 plan，并编译每条 tile 指令及其固定读取授权。执行协调器按 plan 顺序执行这些指令，拥有 Run 的值与状态，并执行回调与 I/O。`ResultRef` 拥有其 schema、单调 descriptor 事实、已认证 backing。引用的副本共享这份所有权。外部引用或读窗口在执行上下文结束后仍让所需 backing 保持存活。依赖事实不属于 Result：执行把每个已执行 tile 的已编译读取记录在 `TileRecord` 中（见[Tile 记录与冻结证据](#tile-记录与冻结证据)）。

```cpp
struct ResultTensorLayout final {
  bool spatial = false;
  std::uint32_t height_axis = 0, width_axis = 1;
  std::optional<std::uint32_t> channel_axis = 2;
};

struct ResultTensorSpec final {
  ResourceLease metadata_owner;
  ResourceString key;
  ResourceVector<std::uint64_t> batch_axes;
  std::uint32_t atomic_trailing_axes = 0;
  TensorDescriptor descriptor;
  ResultTensorLayout layout;
};

enum class ResultPublicationPolicy : std::uint32_t {
  CompleteBundle = 1, IndependentChunks = 2, StablePrefix = 3
};
struct ResultPublicationScope final {
  ResultPublicationPolicy policy = ResultPublicationPolicy::IndependentChunks;
  ResourceVector<std::uint32_t> members;
};

struct SchemaTemplate final {
  ResourceString id;
  std::uint32_t version = 1;
  ResourceVector<ResultTensorSpec> tensors;  // 1..16 members
  ResourceVector<ResultPublicationScope> publication_scopes;
};

struct ExecutionBinding final {
  std::string name;
  ResultRef result;
};
```

摘录省略了成员函数；声明位于 `photospider/data/result.hpp` 与 `photospider/execution/execution.hpp`。带 `result_schema` 的 workflow 输入绑定一个拥有所有权的 `ResultRef`；执行在 `ExecutionResult::results` 中返回具名 Result 输出。Tensor 样本坐标由 `batch_axes` 与 descriptor 的 cell 轴拼接而成。`ResultTensorSpec::metadata_owner` 是为该 spec 的托管嵌套容器付费的资源租约；它是预算所有权，不是附着的解释。`id` 与 `version` 命名一个显式结构契约并进入编译身份；内核不会据此分派到某种领域含义。

完整的结构化执行可以为 `ExecutionResult::results` 中的具名 Result 输出计算一个非安全用途的 `result_digest`：按逻辑顺序覆盖规范 schema、已认证 tensor 覆盖与样本字节。它不包含对象身份和物理布局；它不是认证。Fragment 执行令该摘要为空。`ExecutionOptions::maximum_result_digest_samples` 默认为 65,536，限制摘要准入的已认证 tensor 样本数；零表示禁用摘要。扫描前，协调器还检查剩余 Run-work 与 tensor 覆盖计数的可表示性。若完整摘要不满足这些估计，协调器省略摘要并继续返回执行结果。例如，不会仅为生成摘要而逐个逻辑样本遍历一个巨大的已认证 broadcast view。扫描准入后，摘要读取使用有界窗口并消耗 Run-work；扫描期间的取消、过期状态、Root/资源或读取失败仍是执行失败。协调器在返回成功前再次检查取消与过期状态。

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

`ResultBuilder::start` 接受长度为 1..4096 字节的 scope，将其字节复制到 Root 计费的存储，计入复制工作量，并分配新的 ObjectId。`ResultRef::matches_scope` 把已存储的字节与传入字符串做精确比较。执行宿主还会分别校验 Root ownership、结构 schema、producer scope、revision 递增以及发布 coverage；只匹配 scope 不足以准入 Result。这里的 producer scope 与控制成员 tile 可见性的 `SchemaTemplate::publication_scopes` 不同。

在编译执行中，producer scope 是 Run 局部的调用 key，由 plan digest、Run、block activation，以及指令的 call、stage、输出、成员和 tile 派生。它标识一次发布尝试；内容复用使用下文独立的缓存 identity。物理页大小、文件偏移、descriptor revision 和资源上限不改变语义身份。`ResultRef::capture()` 在某个 revision 捕获已认证 descriptor，因此早期 capture 的覆盖不会随生产进度扩大。`ResultRef::read_tensor()` 使用该 descriptor 并仅读取已认证样本。`ResultDescriptor` 为最多 16 个成员分别保存覆盖 `Footprint`。`ResultTensorInput` 把已编译授权、成员 slot、descriptor 与授权的样本集带入回调；phase 仅在一次 poll 中借用，拥有型 capability 遵循各自的生命周期规则。

`ResultObjectInputs` 另外跨 poll 保存每个输入端口当前拥有的 ResultRef；指令完成或退役时 host 清空该映射。已存储的 `ResultTensorInput` capability 拥有其 ResultRef 与捕获 descriptor，获取的读窗口拥有该授权覆盖的 backing。物理 tensor view 独立保留其来源 Result、schema 与 backing。

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

C++ vector 重载没有固定的 piece 数量上限，其规模由 Root 的 work 与 metadata 准入约束；C ABI 7 bridge 接受 1..16 片。piece vector 为空、`source` 为 NULL、piece 中 `reshape=true`，或某片区域超出 `total_region`、与其他片重叠、未覆盖 `total_region` 的一部分时，返回 sticky `TypeMismatch`。

Builder 先校验完整逻辑分区、dtype、来源授权和逐片映射，然后准备物理 spans。之后统一检查所有来源 Result owner（允许来自不同 Roots）的 ownership cycle，并只调用一次 `publish_tensor_spans`。成功时一次认证覆盖并推进 revision，借用窗口释放后仍保留来源 backing。中间 piece 不会单独发布。

View 路径不复制 payload。目标 Root 计入 metadata、几何/work 和 publication；source-backed 字节仍在各来源 Root 计费。直接提供不带 Result 来源 owner 的外部 storage 则通过独立的 `reference(storage)` 准入。

即使某片映射已报告 `ViewUnavailable`，cycle 检查仍会执行；来源链回到目标 Result 时返回 sticky `TypeMismatch`，不提供复制重试。逻辑授权、覆盖重叠、owner cycle、资源失败与取消均不能作为自动复制回退；仅物理不可表示但逻辑合法的映射返回非 sticky 的 `InvalidArgument` 与 `FailureReason::ViewUnavailable`，保留既有 coverage/revision 和早期 capture，算子才可以选择显式物化。其他错误为 sticky；失败或取消的事务不暴露部分组装的视图。

## 调度与发布

### 计算单元与 tile 网格

每个 plan step 以各自解析后的输出契约声明一个选定的具名输出。该输出的每个成员都有自己的固定 tile 网格，成员的每个已编译 tile 都是一条 plan 指令。内核执行没有整体 Result 模式：需要完整输入或输出的算法通过映射的成员 tile 或 Run 共享 stage 表达该需求（见 [Run 共享 stage](#run-共享-stage)）。

`PlanningOptions::tile_extent` 为整个 plan 设置一个正方形 tile extent。默认值为 128，必须是 2 的正整数次幂（包括 1）。`TileGrid::from_tensor()` 依据 schema 与该 extent 为每个成员推导一个网格：

- Spatial 成员按 height 与 width 轴切分 tile。其显式 batch 轴成为网格 batch，每个 tile 只含一个 batch 坐标；其他 cell 轴（如通道轴）在每个 tile 中完整保留。
- 非 spatial 成员按最后两个自由 cell 轴切分 tile，自由轴指 atomic 末尾轴之前的轴。更前面的自由轴（包括显式 batch 轴）成为网格 batch。只有一个自由轴的成员只切分该轴。Atomic 末尾轴在每个 tile 中完整保留。

对形状 `{4, 1080, 1920}`、`atomic_trailing_axes = 0` 的 planar RGBA 图像，`channel_axis = 0` 的 spatial layout 得到 `{4, 128, 128}` 的 tile，因此每个 tile 含其像素的全部四个通道。形状 `{2, 3, 1000}` 的非 spatial Float32 表把 `{3, 1000}` 切为 `{3, 128}` 的列块，并把首轴用作两个 batch。网格创建拒绝 tile 计数溢出，但不要求完整样本数能放入 `uint64_t`。网格与物理存储无关。

```cpp
struct TileUnit final {
  std::size_t step = 0;
  std::uint32_t output = 0, tensor_slot = 0;
  TileIndex tile = 0;
};

struct TileNeed final {
  std::uint32_t input = 0, tensor_slot = 0;
  bool descriptor_only = false;
  TileSet tiles;  // half-open intervals in the producer member's grid
};

struct ResultSharedNeed final {
  std::uint32_t stage;
  std::optional<TileRequest> tiles;  // absent: all stage members
};

struct ResultProgramNeed final {
  ResourceVector<TileNeed> tiles;
  ResourceVector<ResultSharedNeed> shared_stages;
};
```

摘录省略了构造函数与成员函数；声明位于 `photospider/data/tiles.hpp`、`photospider/execution/tile_dependencies.hpp` 与 `photospider/plugin/result_program.hpp`。`TileSet` 存储规范化的半开区间 `[begin, end)`；构造、合并与复制的开销随区间数而非 tile 数增长。`TileRequest` 把成员 slot 映射到 tile 集合；`TileRequest::all()` 依据静态网格构造完整的成员区间。

Planner 为每个可达单元 `(step, shared stage, member, tile)` 生成一条指令，并对同一单元的重复请求去重，因此被多个消费方读取的 tile 在每次 block activation 中只运行一次。指令的 query 携带 `tensor_slot`、`tile_unit`、`tile_extent`，以及等于该单元 tile 区域的 `tensor_outputs` footprint。普通 tile 指令的每次 `ResultPublication` 必须设置 `complete = true`，恰好覆盖所选成员的 query tile，并让其他成员的覆盖为空。部分或非最终发布无法通过校验。Continuation 可以使用多次 poll；渐进式 Result 发布属于共享 stage 执行。

`ExecutionPlan::structured_templates()` 为每个 step 保存一个静态 template，由编译器计算一次。Template 摘要 tile extent、执行模式、后端、operation key、输出索引、解析后的 traits、所选资源 identity、参数和输入 schema。它不含 graph、node、step 或 plan 标识符，因此位于不同 step 的两个等价 call 共享同一个 template。

### Need 与授权

算子的静态 mapper 在编译期返回 `TileNeed` 记录，planner 把它们转换为指令的 `PlanTensorRead` 授权。每个 `TileNeed` 恰有一种形式：

- Payload 记录指定一个输入、一个成员 slot 和成员局部的 tile 区间。Planner 依据该 producer 成员的编译期网格校验区间。Payload 授权同时授权该输入的静态 descriptor 事实。
- Descriptor-only 记录的 `tensor_slot = 0` 且没有区间。它授予该输入的编译期 descriptor 事实，object id 为 0，不授予 payload。
- 区间数为零的 payload 记录合法，表示不请求任何内容。Planner 丢弃它：不创建依赖、不授予 descriptor，也不授予样本。

Planner 合并同一输入与成员的记录，把每个授权按 publication scope 和完整校验要求闭合，并把 producer tile 记录为指令依赖。运行时，协调器把被授权的 producer tile 组装为一个由 tensor view 构成的 Result，并授予这些区域的并集，该并集按 `atomic_trailing_axes` 闭合。Callback 仍可以返回 `ResultProgramNeed`，但协调器只检查其记录位于这些授权之内，从不启动 producer。指令完成时，协调器把其授权的并集、本次 activation 的控制证据以及其读取的 block 参数证据安装为该 tile 的 `TileRecord`。区间只在同一 producer 与成员内合并；记录不保留 payload。`ExecutionDependencies::tile_record()` 返回已完成 tile 的记录，`dirty_units()` 从变化的 producer tile 或 descriptor 沿反向边找到消费过它们的已记录 tile。每个具名 return 还在索引 `steps().size()` 加其 return 序号处有一条 synthetic 记录。

完整 tile 的发布是原子的：一个 tile 要么以完整覆盖变为可见，要么完全不可见。仅凭一个 producer tile 完成并不能确立其输出的完整覆盖；覆盖是已发布 tile 的并集。已发布 tile 的依赖记录由其已编译授权固定，之后不能改变。

### Publication scope

`SchemaTemplate::publication_scopes` 在完整成员之上静态声明可见性规则。成员 slot 与 scope 严格有序且互不相交，列出的 slot 至多 16 个。未列入任何 scope 的成员使用 `IndependentChunks`。

| Policy | 成员 | 可见性 |
| --- | --- | --- |
| `IndependentChunks` | 任意 | 每个完成的 tile 独立变为可见。 |
| `CompleteBundle` | 一个或多个 | 所列成员的每个 tile 全部完成后，这些成员一起变为可见。 |
| `StablePrefix` | 恰好一个 | 完整 tile 按局部 tile 索引顺序变为可见。 |

Scope 控制可见性，不控制读取权限。Planner 把它们转换为已编译闭包：触及某个 `CompleteBundle` 成员的读取依赖整个 bundle 的每个 tile，包括未被请求的成员；读取 `StablePrefix` 的某个 tile 依赖其之前的每个 tile。协调器在发布时还校验预期 schema、Root ownership、producer scope、revision 递增和输出 tile coverage；仅 producer scope 匹配不足以准入一个 Result。聚合校验与依赖认证先于消费方可见；bundle 成员不会提前可见。返回的 Result 仍只包含被请求的选择。

设置 `ExecutionOptions::result_publication` 时，所有 scope 都是 `IndependentChunks` 且没有完整覆盖输出校验器的输出，会在每条指令认证时逐 tile 校验并通知，包括从 completed-result 缓存采用的 tile。校验或 observer 失败会在下一条指令之前停止 Run。`CompleteBundle`、`StablePrefix` 和完整校验输出在 call 的已编译聚合认证后通知一次。被通知的对象与之后以输出名返回的投影对象不一定共享 `object_id`；消费方应比较覆盖范围、内容和 owner 语义。

### Run 共享 stage

有些算法需要读取完整输入、执行一次全局遍历，或只产生一次外部可见副作用。算子把此类工作声明为 `OperationSharedStage`：

```cpp
struct OperationSharedStage final {
  std::string key;
  std::vector<ExtensionIdentity> validators;
  SchemaTemplate result_schema;
  std::vector<std::uint32_t> input_indices;
  Backend backend = Backend::Cpu;
  std::uint64_t continuation_bytes = 0, workspace_bytes = 0;
  std::uint64_t maximum_work = 0, maximum_output_payload_bytes = 0;
  std::uint32_t maximum_dependency_stages = 0;
  bool mandatory = false;
};
```

Stage 契约是静态的。准备阶段固定其 schema、输入投影、后端、状态、workspace、work 与输出 payload 上限，这些都进入 operation 身份。Planner 把 stage 单元规范化到该 call 的第一个 step，因此同一 call 的所有输出和 tile 到达相同的 stage 指令。在一次 block activation 内，协调器为每个 call 和 stage 保留一份 stage 状态；再次进入该 block 时清空它，因此每轮循环都会重新运行其 stage。输入投影限制 stage 的 mapper 可以指名哪些输入，不授予读取权限。Stage 只读取它自己的 mapper 产生的授权。所有 stage 上限都是该 stage 在一次 activation 内的累计值，work 上限包括输入校验器与发布校验器。

只有当消费方的 mapper 为某个 stage 返回 `ResultSharedNeed` 时，消费方才读取该 stage。`tiles` 选择缺省时请求该 stage 的每个成员 tile；显式空选择不授予 payload。Planner 按 stage 的 publication scope 和完整校验要求闭合该选择，并让消费方依赖这些 stage 指令，它们在同一 block 中更早运行。Stage 单元与 tile 单元之间的依赖环使 planning 以 `Cycle` 失败。每次可见的 stage 提交都在提交时冻结其依赖证据。消费方获得限定在其授权内的 stage Result 和这份冻结历史，但不获得读取该 stage 原始输入的权限。

Stage 失败会使执行它的指令失败，从而使 Run 失败；取消会在已准入回调返回后使 stage 退役。Stage 不会溢出，也不会跨 Run 共享。Planner 把每个 call 的每个 `mandatory` stage 作为副作用根编译，即使 plan 没有选择任何输出 tile，`select_outputs` 也会保留它们；只要包含其 call 的 block 执行，mandatory stage 就会运行。非无副作用的算子必须声明至少一个 mandatory stage；不同时满足确定性与无副作用的算子不能声明为可缓存。一个算子至多声明 64 个 stage。

在 `ExecutionMode::CpuExact` 下，planner 为 GPU stage 生成指令时以 `BackendUnavailable` 失败。只读取 stage 元数据的 plan 不受影响。当普通 step 或 stage 使用 GPU 后端时，Run 创建其 native 上传状态。GPU stage 没有编译期 CPU 回退；stage 算术从不在另一后端上重试。

直接调用的 stage `ResultContinuation` 遵循相同的静态可见性规则。隐藏的中间提交返回空 Need，以便 host 再次 poll。返回的非最终发布是某个 revision 上的 `capture()`，因此之后被拒绝的增量不会通过更早的 `ResultRef` 出现。若校验器拒绝后续 tile，host 以该失败使投影 producer 退役：已逃逸前缀的 `production_status()` 在 continuation 销毁前后都报告该失败，而此前已认证的 tile 仍可读取。

### SDK 获取辅助函数

C++ SDK 头文件 `photospider/plugin/tensor_helpers.hpp`、`tile_helpers.hpp` 和 `whole_stage.hpp` 提供非阻塞辅助函数。每个辅助函数要么返回一个 Need 供回调从 poll 中返回，要么返回就绪值；它们都不会执行 producer、等待、填充缺失样本或隐式收集稠密字节。在静态 VM 下，授权在第一次 poll 前已经绑定，因此辅助函数通常在第一次调用时就返回就绪值；返回的 Need 只声明已编译授权。`tile_helpers.hpp` 还提供静态 mapper 辅助函数 `sdk::static_query_view`、`sdk::tile_need_for_samples(const ResultStaticQuery&, ...)` 和 `sdk::pointwise_dependencies`。

| 辅助函数 | 就绪值 | 用途 |
| --- | --- | --- |
| `sdk::to_whole(phase, input)` | `WholeTensorInputs`，每个成员一个完整 capability | 获取一个输入的全部成员。 |
| `sdk::to_whole(phase, input, slot)` | 覆盖该成员的 `WholeTensorWindows` | 以拥有型读窗口获取一个完整成员。 |
| `sdk::to_whole(phase, input, slot, samples)` | 按 `samples` 每个 box 裁剪的 `WholeTensorWindows` | 获取成员 ROI。`samples` 为空时返回空的就绪向量且不授予任何内容。 |
| `sdk::to_tile(phase, source, member)` | 覆盖 query tile 的零拷贝 view 组成的 Result | 从拥有的 Result 或 stage Result 发布所选输出 tile。 |
| `sdk::collect(phase, input, region)` | `TensorSpan` | 显式稠密复制一个已授权矩形。 |
| `sdk::publish_dense_tensor(...)` | 通过 `ResultBuilder` 发布 | 为一个矩形显式分配稠密输出。 |

`to_whole` 的 ROI 重载在入口拒绝已取消的 query；只要有被选样本位于已绑定授权之外，就返回紧凑的 tile Need，已编译授权足够时立即就绪。完整获取不意味着稠密收集；稠密复制是算子的显式决定。

`sdk::with_whole_stage(definition, work_bound)` 把一个算法需要完整输出的单输出 CPU 算子适配为一个 Run 共享 stage。它包装 definition 的 mapper，使 stage 获得原始输入读取，而每个输出 tile 读取完整 stage。当 block activation 执行该 stage 时，原 factory 以完整输出 schema 且没有所选 tile 运行一次；它仍负责自己的数值策略和资源上限。随后输出 tile 回调通过 `sdk::shared_result_tile` 发布 stage Result 的保留 view。这是算子作者选择的 SDK 适配器，不是内核执行模式。有多个独立输出的算子使用各自的 stage 和更窄的输入投影；输出需联合计算的算子（如稳定排序）使用一个联合 stage。

### Tile 记录与冻结证据

```cpp
struct TileDependency final {
  PlanInput producer;
  std::uint32_t producer_output = 0, tensor_slot = 0;
  TileSet tiles;  // empty: descriptor-only
};

struct TileEvidence final {
  std::uint64_t identity;
  TileUnit owner;
  ResourceVector<TileDependency> inputs;
  ResourceVector<std::shared_ptr<const TileEvidence>> inherited;
};

struct TileRecord final {
  TileUnit unit;
  ResourceVector<TileDependency> inputs;
  ResourceVector<std::shared_ptr<const TileEvidence>> shared;
};
```

每个 payload 依赖也观察 descriptor 变化。`TileEvidence` 是一次 stage 提交的不可变依赖证据：它拥有该提交的输入授权，并引用更早的冻结证据。它从不是读取 capability。Stage tile 的消费方只保留对该节点的一个引用。执行表把每个不同节点导入目标 Root 一次并保留其身份，为每个节点只建立一次反向边索引，并从一个加锁的表状态枚举 workflow 依赖。节点按引用计数退役。这些规则避免把 P 个 stage tile 的证据复制到 Q 个消费方中的每一个。

### Mandatory 副作用与重复输入

Planner 把每个非 side-effect-free 的 call 和每个 mandatory 共享 stage 作为副作用根编译，包括 plan 的输出选择为空的情况。`select_outputs` 和显式 `DemandQuery` 保留这些根，以及每个分支条件和边参数，因此空选择会省略纯输出计算，而实际执行路径上的控制流与 mandatory 副作用仍会运行。未被选中分支中的副作用不会运行。一个 block 中的 mandatory stage 在该 block 的 terminator 求值之前完成。有副作用的工作不跨 Run 共享或缓存。已执行的 mandatory stage 失败或取消会使 Run 失败。

Result C ABI operation 定义可以声明一个固定输入前缀和一个重复的尾部组。`input_count` 与 `inputs` 只描述前缀；`repeated_input` 提供原型约束，在元数据解析和 `start`/`poll` query 校验时应用于每个实际尾部输入。非零 `repeated_maximum` 启用该组，并要求 `1 <= repeated_minimum <= repeated_maximum`，前缀加最大值不超过 1,024 个输入。禁用时，所有重复字段为零或 null。`repeated_match` 为 0 或 1：取值 1 要求每个重复 tensor 与第一个重复 tensor 的 dtype 和完整逻辑 `sample_shape()` 一致；取值 0 要求为展开后的输入列表提供纯元数据解析器。C++ registry 保留未展开的模板，并在准备期间解析实际输入数。该声明只约束元数据；算子的 mapper 仍要授权它读取的每个实际输入。

### Native GPU 输入访问

Native GPU 回调可以对已编译的 payload 授权调用 `ResultProgramPhase::acquire_native_atlas(input, slot)`。它返回一个不可变 `FragmentAtlas`，恰好包含该授权的覆盖；如何 dispatch 与发布仍由算子决定。一次 poll 内对同一端口和 slot 的调用复用同一个 atlas。CPU 访问、缺失授权或 descriptor-only 授权会产生 sticky `InvalidArgument`，且不扩大授权。Root 支持的 payload 与目录存储独立于算子 workspace，atlas 拥有这些缓冲区，但不保留来源 Result 或读窗口。调用方持有的共享 atlas 可以在 poll、来源与执行上下文结束后继续存活。

Native GPU 输入传输仅当弱证明仍标识同一个存活的 CPU 存储且完整 view key 匹配时，才在 Run 内复用原始物理 backing。可选的按字节有界上下文缓存可以按设备实例/构建以及逻辑输入位、Region、dtype 和形状跨 Run 复用上传；它不包含 Result schema，命中后重新绑定到当前 schema。缓存哈希消耗协调器共享的缓存 work 预算，可选 work 耗尽时跳过缓存路径。容量回收先清除可重传的 Run 局部条目，再逐出保留的上下文条目。`host_access_count` 在一个回调范围内对每个存储只记录一次存活 native 支持存储的实际 CPU 暴露；元数据检查、native 地址获取、view 转发和哈希不计数。Native GPU 服务失败在服务调用返回时锁存。之后的 `consume_work` 耗尽或回调状态不能替换这个更早的服务错误；若 work 耗尽先发生，之后的服务错误也不会替换它。

Result atlas 打包接受 `Tensor` 或 `ResultTensorInput` capability。底层的 `FragmentAtlasPlan` 可以在不读取来源样本的情况下打包覆盖为空的 `Tensor`。Result 重载通过已授权的拥有型窗口读取，因此无需稠密转换即可处理有符号与 broadcast stride、碎片化覆盖和带 batch 前缀的 spatial tensor。`preparation_work()` 与 `materialization_work()` 报告通用的几何与打包单位。Result 准备通过 `FootprintLimits::consume_work` 预扣其通用 work；Result 物化还通过该回调预扣窗口获取与读取上限 work。`materialization_work()` 不是 Result 重载的完整扣费。

### 跨输出 block 状态共享

当当前请求与证明匹配时，block 状态保留可以复用已完成的状态。保留状态是执行实现细节；缓存命中数与 block 转换不是公开的性能保证。

纯确定性 Result 算子可以选择启用 `share_blocks_across_outputs`；对不同时满足确定性与无副作用的算子，注册拒绝该选项。C Result ABI 7 以 `PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V7`（标志位 5）提供相同选项。

共享命名空间包括算子及其所有解析后的输出契约、后端、执行模式、所选资源身份、静态参数和实际输入元数据。每个 block key 还包括其 kind、range、mode、输入状态的 schema/覆盖/内容，以及当前提供的 tensor 元数据、覆盖和字节。所选输出索引与输出特定元数据不会单独附加。因此由不同输出共享的计算必须与输出无关，或把差异编码在状态或 mode 中。公开输出 Result、tile 记录、checkpoint 和 completed-result 缓存仍按输出划分且彼此独立。

共享 stage 不提供 checkpoint 或 block 回调。对普通 tile 指令，block 状态查找与保留要求纯指令：确定且无副作用的算子、对已授权 payload 输入纯的 producer 闭包、纯的控制证据、无 fallback 污染、没有共享 stage 读取，且不读取 block 参数的 payload。Block 复用不要求算子或其祖先可缓存；该要求只适用于 completed-result 保留。Block key 从 block 契约（算子、执行模式、后端、tile extent、非共享时的输出索引、traits、资源、参数与输入 schema）开始，再加入指令的固定授权内容证明（`photospider.fixed-block-inputs.v2`）、block kind、range、mode 和输入状态。保留是可选的，计入有界缓存 work 与 Root 容量。只有当 compute 回调在该 block 期间确实 dispatch 了原生工作时，GPU block 才会被保留，因此没有原生计算证明的 GPU 路径从不被存储。若查找或保留不可用，回调正常计算该 block。若查找命中但缓存状态因容量不足无法解包，则视为 block 未命中并正常计算。解包期间的 work 上限、取消和协议失败仍是致命错误。每个输出仍执行自己的指令并记录自己的 `TileRecord`。该标志不保证每个 Run 只计算一次。

### CPU fallback

CPU fallback 只存在于 planner 编译了它的位置：当放置在 GPU 的算子允许回退、支持 CPU、确定且无副作用、没有共享 stage 读取，并且其 CPU mapping 落在 GPU 授权之内时，指令的 `cpu_fallback` 标志才被设置。之后若 GPU start 或 poll 在任何发布、输入读取、native dispatch、checkpoint、block 或 GPU selection 之前以可重试的 `BackendUnavailable` 失败，协调器为重试提供新的失败锁存，丢弃该尝试的 continuation，并以相同的已绑定授权在 CPU 上重新启动同一条指令。已完成的 producer tile 保留其已发布的 Result 与记录。已消耗的 Root work 不退还。被拒绝的 GPU 尝试保留其 `OperationTiming` 记录，CPU 尝试添加自己的记录。重试的 tile 带 fallback 污染，在可选复用中视为不纯，因此其值以及读取它的每个消费方都跳过 checkpoint 查找与发布、block 状态复用和 completed-result 保留。

### Checkpoint

纯确定性 Result 程序的普通 tile 指令可以使用 C++ `ResultProgramPhase::checkpoint_publish` 与 `checkpoint_before` 回调保留 continuation 状态；从共享 stage 或不同时具备两项纯度 trait 的算子调用任一回调都是协议失败。查找与保留还要求纯指令（producer 闭包与控制证据纯、无 fallback 污染、没有共享 stage 读取、不读取 block 参数的 payload）以及剩余的可选缓存 work。否则查找未命中、发布被拒绝，但不使算子失败。Checkpoint 不要求算子或其祖先可缓存。

Checkpoint 范围是内容 identity。其 key 是指令的固定授权内容 key（`photospider.fixed-grant-cache.v2`，覆盖 step 的 structured template、后端、成员、tile、publication samples、descriptor 授权的端口，以及已授权输入的 schema 与样本字节）加 `/checkpoint` 后缀。它不含物理 step、node 或 plan 标识符，因此位于不同物理 step 的两个等价 call，或同一 call 在重新编号或重新加载的 plan 中，共享同一个范围。`checkpoint_before(phase, before)` 返回该 phase 中不大于 `before` 的最大已发布序号。已发布的 checkpoint 拥有一个已封存的状态 Result 和一个不含 payload 的 witness，witness 保存发布方 tile 的 `TileRecord`。

恢复 checkpoint 只恢复状态内容。当前指令保留自己的固定授权、依赖证据和 `TileRecord`；更早的 witness 从不替换它们，也不会引入另一个 call 的物理 tile 身份。恢复不会填充当前 phase 的 `results` 或 `tensors`，不会重建读窗口，也不授予读取权限；回调通过已绑定的授权读取当前输入。容量拒绝返回空 checkpoint，且不改变该指令。Work 上限、取消和协议失败仍是错误。

上下文目录持有 checkpoint 范围的弱引用，活动执行持有其范围的强引用；`ExecutionContextConfig::maximum_result_checkpoint_scopes` 限制存活范围数。每个范围至多保留 64 个条目，一个范围的 witness 总权重不超过依赖 `maximum_boxes` 上限；最旧条目先退役。运行时在构造查找 key 或复制 witness 记录之前先扣除可选 checkpoint work。可选缓存 work 为零或不足时跳过查找或拒绝发布；不会复制未准入的 witness 数据。复制得到的 `ResultCheckpoint` 在索引或上下文清除后仍拥有其状态与 witness。C Result ABI 通过 poll 作用域的不透明句柄和已封存的通用状态 Result 提供 `checkpoint_before`、`checkpoint_read` 与 `checkpoint_publish`；见 [C checkpoint 服务](Dependency-Data.zh.md#c-checkpoint-服务)契约。

Checkpoint 与 block 资格取决于纯度，与 completed-result 缓存无关：即使算子或其某个祖先以 `cacheable = false` 关闭 completed-result 保留，纯指令仍可使用这两项服务。二者仍受上述指令限制。声明 `deterministic = false` 的算子（例如其输出包含 Run 局部 ObjectId）会使每个依赖它的消费方变为不纯，并把它排除在 checkpoint 查找与保留以及 block 缓存准入之外。

### Completed-result 缓存

当指令的固定输入内容相同时，completed Result 缓存跨 Run、绑定 generation 和等价 plan 复用一条 tile 指令的已封存 Result。查找 key 是固定授权内容 key（`photospider.fixed-grant-cache.v2`）。它摘要 step 的 structured template、指令的后端、成员 slot、tile 索引与 publication samples、descriptor-only 授权的端口，以及每个 payload 授权的端口、成员、规范 schema 和已授权样本字节。它不含物理 node、step 或 plan 标识符，因此重命名、重新编号或重新加载的 plan，或另一 step 上的等价 call，都可以复用条目。编辑后 descriptor 授权指向不同端口的 plan 会产生不同的 key，不会与另一个 plan 的条目碰撞。每个 key 至多保留八个候选，且只考虑后端相同的候选。

匹配的候选以新 ObjectId 和当前 producer scope 重新绑定，并像计算所得发布一样校验，包括输出 payload 上限。输入校验器已在指令的已绑定授权上运行，因此无效的 workflow 输入仍会在使用缓存输出之前以 `InvalidArgument` 失败。随后 Run 安装该 tile，其依赖证据由当前 plan 的固定授权重建；缓存条目只提供内容。缓存命中不产生 `OperationTiming` 记录，`computed_elements` 增加为零。缓存条目保留其引用的物理 backing owner；重新绑定不会让旧 Result 包装或旧执行范围成为新输出的身份。内容 identity 并不使阶段 digest 稳定：semantic、optimized 和 plan digest 包含程序标识符，call 或 value 重新编号时会改变。

结构化 Result 条目共享有界缓存 LRU 与托管 Root 容量。LRU 至多持有 4,096 个条目，`ExecutionContextConfig::result_cache_bytes` 限制保留的 backing（零表示禁用保留与 block 复用），`maximum_dependency_cache_metadata` 限制 manifest 元数据。容量计算对物理 backing owner 去重（包括通过 tensor view 和 affine owner 触及的 backing），而不是累加逻辑 tensor 大小。`ResultCacheStatistics::native_retained_bytes` 统计缓存持有的唯一原生分配，同时覆盖保留的 GPU 输出和保留的 GPU block 状态；它不是仅 block 的容量。逐出会释放缓存持有的 owner 引用；持有 Result 或读窗口的调用方继续拥有其 backing。`clear()` 推进缓存 epoch，因此上一 epoch 中进行中的 producer 不能重新填充已清空的缓存。Key 构造、样本哈希与重新绑定消耗 Run 的可选缓存 work（`ExecutionOptions::maximum_dependency_cache_work`）。可选 work 或 Root 容量不足时跳过保留或产生未命中，不改变本应成功的计算。

保留与查找要求可缓存指令：纯指令（见上文 block 状态共享），其算子为 `cacheable`，其 payload 输入来自 workflow 输入或可缓存的 producer，其控制证据可缓存，且不读取 block 参数的 payload。协调器在 call 的值认证后保留 tile，对单独认证的 tile 则立即保留。共享 stage、带共享 stage 读取的指令、带 fallback 污染的 tile 以及经 block 参数传递的值绕过 completed-result 缓存。C Result ABI 不暴露该缓存；其实现属于 C++ 执行运行时的私有部分。

### Result builder

`ResultBuilder` 拥有发布。Producer 通过 `publish_tensor`、`publish_tensor_view` 或 `publish_tensor_kernel` 写入 tensor 区域。Tensor 区域依据成员的逻辑样本 domain 检查。重叠、越界访问或资源准入失败会以错误使 producer 退役。`ResultBuilder::start()` 接收 query 的 tile extent；非法的 spatial tile extent 在分配前返回 `InvalidArgument`。

发布把物理数组与 backing owner 保存在源自 Result 的存储 owner 中，而发布状态拥有 Root、资源租约与 schema。销毁时先退役数组 backing，再由发布状态释放其 Root 租约。存储 owner 索引避免每次发布都重新扫描此前已发布的 backing；当成员分区允许时，`acquire_tensor` 通过区间查找定位分块，否则使用有界扫描。`acquire_tensor` 在持有发布互斥锁时复制不可变 backing 句柄，然后先解锁再构造拥有型读窗口。`publish_tensor_kernel` 在互斥锁之外准备回调工作，然后重新获取锁，在提交前校验 owner、revision 与 sticky 失败。读取 capability 保留其已授权的 Result 与 backing，但只在捕获的 descriptor 和已编译授权范围内授予读取。

发布校验器在一次同步校验中接收 `PublicationValidationView` 和借用的 `PublicationValidationServices` 接口。协调器拥有取消、work 准入、当前上限和指令状态。校验检查发布身份、revision；它不保留服务，也不执行调度、发布、退役或通知。校验完成后，协调器应用返回的状态与退役意图。

`publish_tensor_kernel` 为被请求区域出借可写窗口。Builder 在成员 tile 与 batch 边界处划分请求，并为 spatial 成员提供 tile 内的 planar 字节。它不预留全 domain backing，因此巨大逻辑 domain 上的稀疏请求按实际可写 tile 计费。回调必须使用每个窗口的 `spec()`、`region()`、`sample_axis()` 以及 `row_run()` 或 `rectangle_run()` 返回的 stride。回调不得假定跨窗口的 planar backing 或连续行。

```cpp
Status ResultBuilder::publish_tensor_kernel(
    std::uint32_t slot, const Footprint& samples,
    const std::function<
        Status(const ResourceVector<ResultTensorWriteWindow>&)>& write,
    const CancellationToken& cancellation = {});
```

Footprint 重载在一个事务中发布一组精确、互不相交的规范 box。Builder 在运行任何 producer 代码之前复制 `samples`，因此修改调用方 footprint 对象的回调不会改变发布集合。只有一个 box 的 footprint 走上文的 Region 路径。对多个 box，footprint 必须已按 tensor 的 observation 分组闭合（`close_samples` 返回同一集合），且不得与已有覆盖重叠；违反时返回 `TypeMismatch`。回调按 `boxes()` 顺序为每个 box 接收一个写窗口。空洞不分配存储，也不获得覆盖。空 footprint 跳过回调与 payload 工作。

回调在发布互斥锁之外运行，运行期间拒绝同一 producer 的修改与封存。回调返回后，builder 重新检查 owner、revision 与 sticky 失败，再检查取消。返回的错误、抛出的异常（分配失败为 `ResourceExhausted`，其他为 `OperationFailed`）、取消或 revision 变化（`Stale`）会记录 producer 失败、释放缓冲区且不认证新的覆盖；此前已认证的前缀仍可读取。通过窗口写入的 worker 必须在回调返回前完成，因为窗口与裸指针在那时失效。成功时，builder 在一个新 revision 上同时提交这些 box 与覆盖。

`ResultTensorWriteWindow::borrow_dense(spec, writer)` 无复制地把显式行主序 `TensorWriter` 作为写窗口出借。它先校验成员的 spatial 拓扑，再以 `TypeMismatch` 拒绝 dtype 或完整样本形状不同的 writer。

`ResultObjectInputs`、`ResultTensorInput` capability 和保留的窗口提供上文所述的活动 owner 与授权路径。物理 tensor view 还保留其发布字节所来自的每个来源 Result。ICC profile 或 OCIO 配置字节等不可变算子资源不是 Result 成员。静态准备返回其需要的显式 `ResourceIdentity` 列表；编译器从传给 `Compiler::compile` 的 `ResourceSet` 中恰好选择这些条目，缺少任一条目即失败，plan 与执行状态保留所选集合。因此 Result 本身不会让颜色配置保持存活。公开的 `ResourceMap` 与 `ResourceVector` 容器还携带其分配器与托管所有权；复制的结果、名称与观察在这些容器及其包含的 `ResultRef` owner 释放前保留 Root 计费的元数据。消费方必须把这些副本与 payload 一并纳入预算。该策略是保守的，因此存活 owner 可能保留超出最终输出所需的 backing。

## 端口校验器与诊断报告

内核为样本合法性和有界报告提供通用扩展点。它不内置领域校验器：算子 SDK 在 `plugins/ops/common/domain/contracts.cpp` 中注册自己的校验器，例如 `photospider/ops/contracts.hpp` 中声明的 `ops.scalar-range`、LUT table 校验器和 LUT report 校验器。

```cpp
struct ExtensionIdentity final {
  std::string key;
  std::uint32_t version = 1;
  std::string implementation;
};

struct ValidationContext final {
  const SchemaTemplate& schema;
  const ResourceBudget& resources;
  const CancellationToken& cancellation;
  bool complete = false;
  std::uint32_t tensor_slot = UINT32_MAX;
  const Footprint* samples = nullptr;
  std::function<Status(std::uint64_t)> consume;
  std::function<const Footprint*(std::uint32_t)> tensor_coverage;
  std::function<Result<ResultTensorReadWindow>(std::uint32_t, const Region&)>
      acquire_tensor;
};

struct ValidationDefinition final {
  std::shared_ptr<const void> owner;
  ExtensionIdentity identity;
  std::uint64_t scratch_bytes = 0;
  bool requires_complete = false;
  std::function<Result<std::shared_ptr<const void>>(
      const ValidationPreparation&)> prepare;
  std::function<Status(const void*, const ValidationContext&)> validate;
};

Status OperationRegistry::register_validator(ValidationDefinition);
Status OperationRegistry::register_diagnostic(DiagnosticDefinition);
```

端口通过 `OperationPortConstraint::validators` 显式绑定校验器，共享 stage 通过 `OperationSharedStage::validators` 绑定。每个条目命名一个已注册的 key、version 与 implementation；registry 拒绝缺失身份、version 或 implementation 冲突、重复 key、格式错误的定义，以及 `freeze()` 之后的任何注册，且不做部分修改。绑定的身份（包括 version 与 implementation）进入 operation 身份，因而进入 plan cache key。地址、回调指针和报告内容不进入。

校验分两个阶段：

- `prepare` 在算子准备期间、registry 锁之外运行一次。它接收端口或 stage 的 schema、静态参数，以及限制为算子准备所选身份的 `ResourceSet`。它返回拥有所有权的不可变状态。
- `validate` 在入口线程上针对每次新授权或发布运行。`ValidationContext` 只暴露 host 授权的读取：`acquire_tensor` 以 `InvalidArgument`/`UnauthorizedRead` 拒绝当前覆盖之外的成员或区域。Work 通过 `consume` 计费，`scratch_bytes` 在进入前准入。校验器不得保留 context 或任何借用对象。

协调器在以下位置调用校验器：

| 位置 | `validate` 可见的覆盖 | `complete` | 失败类别 |
| --- | --- | --- | --- |
| 输入 tensor 授权 | 为一条指令绑定的某输入端口的 payload 授权，在其第一次 poll 之前。 | 输入 schema 的每个成员都被完整授予。 | Workflow 输入：带外部 `input_id` 的 `InvalidArgument`。产出输入：带上游 `node_id` 的 `OperationFailed`。Origin `Domain`。 |
| 输出发布 | call 的值首次组装时的已编译聚合，或在逐 tile 通知时每个已认证 tile。 | 已校验覆盖等于每个成员的完整 domain。 | `OperationFailed`，origin `Domain`。 |
| 共享 stage 提交 | 每次新可见的 stage 提交，在供给消费方之前。 | 可见的 stage 覆盖完整。 | `OperationFailed`，origin `Domain`。 |

带 `requires_complete` 的校验器只看到完整覆盖；不完整的 context 返回 `TypeMismatch`。完整性在编译期确定。当消费方端口带有完整输入校验器时，planner 把该输入每个成员的每个 tile 都授予读取它的任何指令。当 producer 输出带有完整输出校验器时，planner 让对该输出的任何非空读取都依赖 producer 的每个 tile，因此校验器观察的是全部已执行 tile 的实际已编译聚合，从不根据不完整发布推断额外的 producer 工作。Descriptor-only 与空 payload 授权不增加完整性。完整 stage 校验器把可见性推迟到 stage 静态覆盖完整之后；以不完整覆盖结束的 stage 失败。已存授权缺少所需完整覆盖的 plan 在导入验证时失败。

Completed-result 缓存命中仍会在指令的已绑定授权上运行输入校验器，但不重复计算。因此无效的 workflow 输入会在返回缓存输出之前以 `InvalidArgument` 失败；校验器据以准备的静态参数发生变化时，会产生不同的 plan cache key。

一次 `validate` 调用中的第一个服务失败具有优先权：若读取失败而回调忽略它，该调用仍返回那个读取失败。失败的校验器服务不能发布结果。`prepare` 或 `validate` 抛出的异常变为 typed 失败。`ValidationDefinition::owner` 保留回调代码；host 只在回调对象与已准备的校验器状态之后销毁它，因此调用方释放 registry 句柄后，已准备的算子仍可工作。

`OperationTraits::diagnostics` 列出算子可能发出的报告类型。回调运行前，host 为这些类型初始化一个 Root 拥有的 `DiagnosticAccumulator`。回调按声明序号通过 `ResultProgramPhase::report_diagnostic`（C：`report_diagnostic`）报告一条定长字节记录；`DiagnosticDefinition::validate` 检查它，`merge` 写入一条完整的新记录，不改变任一输入。失败的报告不改变已累积字节。`OperationTiming::reports` 携带拥有所有权的累加器；`find(identity)` 返回合并后的字节，若该类型从未报告则返回 `NotFound`。内核不解释这些字节。数值 work 报告就是这样一种类型，定义在 SDK 头文件 `photospider/ops/numeric/diagnostics.hpp` 中。

算子可以为失败附加不透明的领域代码。`Status(ErrorCode, message, DomainErrorCode, FailureDetail)` 把它存储在 `FailureDetail::domain_code` 中；C 回调在 run 或 validation 服务上使用 `fail_domain(status_code, domain_code)`。内核保留该代码而不解释，第一个记录的领域失败不会被之后的回调或诊断失败替换。

## 限制与失败处理

每个 `ExecutionContext` 创建一个托管 `ResourceBudget`。若未设置 `ExecutionContextConfig::managed_resources`，上下文使用默认构造的 `ResourceLimits`；显式值提供配置的容量。`maximum_live_bytes` 同时限制 Payload 维度。`ExecutionContext::resource_budget()` 为需要计费临时存储的客户端返回一个共享句柄，其租约可以比上下文存活更久。

执行 Root 在配置的资源维度下计费 payload/backing、work、stage、continuation、队列条目、元数据和保留的 owner。上限都是有限的。过大的 payload 或 owner 准入以资源错误失败，而不是返回不完整的 witness。这一托管容量模型覆盖已插桩的资源；它不是 RSS 上限。

输出可以声明 `maximum_output_payload_bytes`，共享 stage 声明自己的上限。Host 在一个 Run 内对一个逻辑输出的各 tile 指令累计新拥有的 payload，每个唯一物理 owner 只计一次。它排除借用的已授权 owner：通过已暴露 Result、已授权 tensor 授予或已提供的 stage Result 仍存活的来源分配。检查只保留来源的弱引用，因此上限不会让输入 payload 保持存活。第一次符合条件的 poll 在进入回调前从 Result Root 分配上限守卫及其共享控制块；Metadata 容量不足时在进入回调前拒绝该 poll。Owner 枚举与比较消耗普通的 Run 与 Root work。超出上限返回带 `CapacityLimit` 的 `ResourceExhausted`。Workspace、元数据与算子资源准入保持独立。检查在 Result 发布时运行，因此可信回调更早的 Root 分配仍被计费，且不会被该上限阻止。

Result 发布错误是 sticky 的。取消与过期执行只在已准入回调返回后才使活动状态退役。失败的追加或 tensor 发布不能被之后成功的回调修复；此前已认证的前缀保持不可变，并可在其捕获的 descriptor 下读取。最后一个拥有型 Result/读窗口释放时，backing随之退役。

行为回归测试随内核构建注册。`tests/unit/kernel` 与 `tests/integration/kernel` 下的注册是可执行覆盖的当前来源。
