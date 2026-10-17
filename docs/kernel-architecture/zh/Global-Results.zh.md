# 结构化 Result 与 tensor 成员

英文权威文档：[Global-Results.md](../Global-Results.md)。

## 模块边界与所有权

`ResultRef` 是全部 workflow 数据（包括图像数据）的发布、ownership 和 input/output 路径。Result 是由 1..16 个 typed Tensor 成员组成的静态目录。成员可以声明 spatial layout；这不创建独立的图像 Result 路径。`Tensor`、`TensorSpan` 与 `TensorWriter` 是内核与 SDK 内部的存储载体，不是 workflow 或 operation 的 input/output 形式。

Result 只携带结构、数据以及存储所需的 owner。它没有附着的领域描述、semantic kind 或 facet 列表。内核不会把颜色、alpha、transfer 或采样描述从输入传播到输出，也不会在连线上匹配这类标签。结构相同的两个 Result 可以连接到同一个端口，即使调用方赋予它们不同含义。需要领域解释的算子以显式静态参数或显式的算子领域配置接收该解释，并可通过绑定的校验器检查样本合法性（见[端口校验器与诊断报告](#端口校验器与诊断报告)）。

当前 package 为 0.36.0。WorkflowDocument schema 为 7，OperationTraits version 为 28，Result C operation ABI 为 6，CPU parallel 与 CPU tiles 服务为 version 2，native GPU 服务为 version 1。这些版本号描述彼此独立的契约。C++ operation interface 使用 `start_result` 与 `ResultContinuation`。Result C operation table 是唯一的 C operation-plugin 接口；consumer 必须针对已安装 package 重建，因为 C++ API 与精确大小的 C 声明是契约。[ADR 0022](../../adr/zh/0022-freeze-operation-c-abi-v3.zh.md)（Proposed）记录所有 operation 共用的按前缀协商 C ABI；package 0.36.0 尚未实现。

编译器把不可变 schema 复制到 plan。执行协调器拥有 producer 状态，并调度回调与 I/O。`ResultRef` 拥有其 schema、单调 descriptor 事实、已认证 backing。引用的副本共享这份所有权。外部引用或读窗口在执行上下文结束后仍让所需 backing 保持存活。依赖事实不属于 Result：执行把每个计算单元实际的 Need 记录在 `TileRecord` 中（见[计算单元与 tile 记录](#计算单元与-tile-网格)）。

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

`ResultBuilder::start` 接受长度为 1..4096 字节的 scope，将其字节复制到 Root 计费的存储，计入复制工作量，并分配新的 ObjectId。`ResultRef::matches_scope` 把已存储的字节与传入字符串做精确比较。执行宿主还会分别校验 Root ownership、结构 schema、actor scope、revision 递增以及发布 coverage；只匹配 scope 不足以准入 Result。这里的 actor scope 与控制成员 tile 可见性的 `SchemaTemplate::publication_scopes` 不同。

在编译执行中，actor scope 编码 structured template（operation 契约、参数与有序输入）、冻结的 snapshot identity 以及计算单元。物理页大小、文件偏移、descriptor revision 和资源上限不改变语义身份。`ResultRef::capture()` 在某个 revision 捕获已认证 descriptor，因此早期 capture 的覆盖不会随生产进度扩大。`ResultRef::read_tensor()` 使用该 descriptor 并仅读取已认证样本。`ResultDescriptor` 为最多 16 个成员分别保存覆盖 `Footprint`。`ResultTensorInput` 把 tensor Need、成员 slot、descriptor 与授权的样本集带入回调；phase 仅在一次 poll 中借用，拥有型 capability 遵循各自的生命周期规则。

`ResultObjectInputs` 另外跨 poll 保存每个输入端口当前拥有的 ResultRef；actor 完成或退役时 host 清空该映射。已存储的 `ResultTensorInput` capability 拥有其 ResultRef 与捕获 descriptor，获取的读窗口拥有该 Need 授权的 backing。物理 tensor view 独立保留其来源 Result、schema 与 backing。

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

`ResultBuilder::publish_tensor_view(slot, region, source, transform)` 接收借用的、已获 Need 授权的 `ResultTensorReadWindow` 与显式 `ResultTensorViewTransform`。`reshape=false` 时，`source_axes` 必须为包括 batch 轴在内的每个完整来源样本轴提供一个 `ResultViewAxis`。映射根据目标**绝对坐标** `output` 计算 `source = source_origin + step * (output - output_origin)`，并检查算术边界。`output_axis=-1` 或 `step=0` 时，来源坐标固定为 `source_origin`。坐标包含目标区域的非零 offset：例如 `source_origin=10`、`step=-2`、`output_origin=5`，则目标坐标 5、6 分别读取来源坐标 10、8。恒等、通道置换与广播使用同一显式映射。

`reshape=true` 时 `source_axes` 为空，完整来源窗口与目标的逻辑样本数一致，且物理的有符号/零 stride 块必须能够合并成仿射映射。逻辑合法但物理布局需要复制时，可返回 `ViewUnavailable`。

多来源时，每个 `ResultTensorViewPiece` 提供一个借用窗口、一个目标坐标中的非空区域以及显式 affine transform。`publish_tensor_view(slot, total_region, pieces)` 至少需要一个 piece；各区域互不重叠、位于 `total_region` 内且并集恰好覆盖它。

**Atomic closure 约束整个 `total_region` 和所有 pieces 的并集，不要求每个 piece 单独闭合。** 例如形状 `[1,2]`、最后一轴为 atomic 的目标，允许两片分别填入通道 `[0,1)` 与 `[1,2)`，两片各自都不是完整的双通道 tuple，但联合后完整。每片仍必须分别通过来源授权和仿射映射检查；多片重载不支持 reshape。

C++ vector 重载没有固定的 piece 数量上限，其规模由 Root 的 work 与 metadata 准入约束；C ABI 6 bridge 接受 1..16 片。piece vector 为空、`source` 为 NULL、piece 中 `reshape=true`，或某片区域超出 `total_region`、与其他片重叠、未覆盖 `total_region` 的一部分时，返回 sticky `TypeMismatch`。

Builder 先校验完整逻辑分区、dtype、来源授权和逐片映射，然后准备物理 spans。之后统一检查所有来源 Result owner（允许来自不同 Roots）的 ownership cycle，并只调用一次 `publish_tensor_spans`。成功时一次认证覆盖并推进 revision，借用窗口释放后仍保留来源 backing。中间 piece 不会单独发布。

View 路径不复制 payload。目标 Root 计入 metadata、几何/work 和 publication；source-backed 字节仍在各来源 Root 计费。直接提供不带 Result 来源 owner 的外部 storage 则通过独立的 `reference(storage)` 准入。

即使某片映射已报告 `ViewUnavailable`，cycle 检查仍会执行；来源链回到目标 Result 时返回 sticky `TypeMismatch`，不提供复制重试。逻辑授权、覆盖重叠、owner cycle、资源失败与取消均不能作为自动复制回退；仅物理不可表示但逻辑合法的映射返回非 sticky 的 `InvalidArgument` 与 `FailureReason::ViewUnavailable`，保留既有 coverage/revision 和早期 capture，算子才可以选择显式物化。其他错误为 sticky；失败或取消的事务不暴露部分组装的视图。

## 调度与发布

### 计算单元与 tile 网格

每个 plan step 以各自解析后的输出契约计算一个选定的具名输出。该输出的每个成员都有自己的固定 tile 网格，每个被请求成员的每个 tile 都是一个计算单元。内核执行没有整体 Result 模式：需要完整输入或输出的算法通过普通的成员 tile 请求或 Run 共享 stage 表达该需求（见 [Run 共享 stage](#run-共享-stage)）。

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

协调器为每个计算单元运行一个 actor。Actor key 拼接 step 的 structured template、冻结 snapshot 身份，以及 step 索引、输出索引、成员 slot、tile 索引与 tile extent 的摘要。Template 是所选输出契约及其上游 template 或 workflow 输入名称与 schema 的静态摘要；它不含 graph、node 或 step ID。在一个 Run 内，对同一单元的重复请求复用其 actor。索引不同的 step 即使 template 相同也保持各自的 actor；只有下文的 completed-result cache 能复用相同 template。Actor 的 query 携带 `tensor_slot`、`tile_unit`、`tile_extent`，以及等于该单元 tile 区域的 `tensor_outputs` footprint。普通 tile actor 的每次 `ResultPublication` 必须设置 `complete = true`，恰好覆盖所选成员的 query tile，并让其他成员的覆盖为空。部分或非最终发布无法通过校验。Need 可以跨多次 poll；渐进式 Result 发布属于共享 stage 执行。

### Need 与授权

回调返回 `ResultProgramNeed` 来请求输入数据。每个 `TileNeed` 恰有一种形式：

- Payload Need 指定一个输入、一个成员 slot 和成员局部的 tile 区间。Host 依据该 producer 成员的编译期网格校验区间。Payload Need 同时授权该输入的静态 descriptor 事实。
- Descriptor-only Need 的 `tensor_slot = 0` 且没有区间。它授予该输入的 descriptor 事实，不授予 payload。
- 区间数为零的 payload Need 合法，表示不请求任何内容。Host 跳过它：不创建依赖、不授予 descriptor、不授予样本，也不启动 producer。

Host 在启动任何 producer 工作之前校验完整的 Need envelope，包括区间数和成员 slot。对每个 payload Need，协调器获取每个被请求 producer tile 的 actor，把各单元的 Result 组装为一个由 tensor view 构成的 Result，并授予被请求 tile 区域的并集，该并集按 `atomic_trailing_axes` 闭合。单元完成时，协调器把其已准入 Need 的并集安装为该单元的 `TileRecord`。区间只在同一 producer 与成员内合并；记录不保留 payload。`ExecutionDependencies::tile_record()` 返回已完成单元的记录，`dirty_units()` 从变化的 producer tile 或 descriptor 沿反向边找到消费过它们的已记录单元。

完整 tile 的发布是原子的：一个单元的 tile 要么以完整覆盖变为可见，要么完全不可见。仅凭 producer 单元完成并不能确立其输出的完整覆盖；覆盖是已发布 tile 的并集。单元的实际 Need 随其发布一起冻结，因此之后的 Need 不能改变已发布 tile 的依赖记录。

### Publication scope

`SchemaTemplate::publication_scopes` 在完整成员之上静态声明可见性规则。成员 slot 与 scope 严格有序且互不相交，列出的 slot 至多 16 个。未列入任何 scope 的成员使用 `IndependentChunks`。

| Policy | 成员 | 可见性 |
| --- | --- | --- |
| `IndependentChunks` | 任意 | 每个完成的 tile 独立变为可见。 |
| `CompleteBundle` | 一个或多个 | 所列成员的每个 tile 全部完成后，这些成员一起变为可见。 |
| `StablePrefix` | 恰好一个 | 完整 tile 按局部 tile 索引顺序变为可见。 |

Scope 控制可见性，不控制读取权限，也不控制回调调度。协调器在发布时还校验预期 schema、Root ownership、actor scope、revision 递增和输出 tile coverage；仅 actor scope 匹配不足以准入一个 Result。当请求触及某个 `CompleteBundle` 的成员时，可见性闸门驱动整个 bundle 的每个 tile，包括未被请求的成员。聚合校验与依赖认证先于一次原子安装完成；bundle 成员不会提前可见。Bundle 的所有单元继承同一份冻结的 bundle 依赖并集，任一成员的 fallback 来源都作用于每个成员，因此读取单个成员的 sink 不能把与带 fallback 污染的兄弟成员一起认证的结果当作干净结果。`StablePrefix` 闸门在可见后仍保留其 producer actor，因此之后对更早 tile 的请求复用它们已认证的状态。返回的 Result 仍只包含被请求的选择。

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

Stage 契约是静态的。准备阶段固定其 schema、输入投影、后端、状态、workspace、work 与输出 payload 上限，这些都进入 operation 身份。在一个 Run 内，协调器为每个原始 node 实例与 stage 至多创建一个 producer，与请求它的 tile 或输出数量无关。输入投影声明潜在依赖，不授予读取权限。Stage 在读取前发出普通的成员 Need。所有 stage 上限都是该 stage 在该 Run 内的累计值，work 上限包括输入校验器与发布校验器。

消费方通过 `ResultSharedNeed` 请求 stage。`tiles` 选择缺省时请求该 stage 的每个成员 tile；显式空选择不授予 payload，也不启动 producer。协调器先尝试从不可变的已提交 stage 覆盖中授予被请求的 tile。只有缺失的覆盖才登记等待边并推进 producer。因此，正在等待另一个 stage 的 continuation 仍可作为其已提交 tile 的有效来源；需要在未提交 tile 之间形成环的等待返回 `Cycle`。每次可见的 stage 提交都在提交时冻结其依赖证据与 fallback 来源。消费方获得 stage Result 和这份冻结历史，但不获得读取该 stage 原始输入的权限。之后被污染的提交不会回溯污染更早的干净提交。

Stage 的成功或失败保留到 Run 结束。失败的 stage 使请求受影响 tile 的每个消费方失败，Run 取消时 producer 在已准入回调返回后退役。Stage 不会溢出、在 Run 内不会重算，也不会跨 Run 共享。普通执行运行每个 `mandatory` stage，即使 plan 没有选择任何输出 tile。使用显式 `DemandQuery` 的执行仅在其全部选择为空时跳过 mandatory stage。非无副作用的算子必须声明至少一个 mandatory stage；不同时满足确定性与无副作用的算子不能声明为可缓存。一个算子至多声明 64 个 stage。

当 `ExecutionMode::CpuExact` 下的执行请求 GPU stage producer 时，协调器在该处返回 `BackendUnavailable`。只读取 stage 元数据的 plan 不受影响。当普通 step 或 stage 使用 GPU 后端时，Run 创建其 native 上传状态。CPU fallback 仅限于 stage 计算开始之前的准入阶段；进入 stage factory 后，不会通过切换后端重试 stage 算术。

直接调用的 stage `ResultContinuation` 遵循相同的静态可见性规则。隐藏的中间提交返回空 Need，以便 host 再次 poll。返回的非最终发布是某个 revision 上的 `capture()`，因此之后被拒绝的增量不会通过更早的 `ResultRef` 出现。若校验器拒绝后续 tile，host 以该失败使投影 producer 退役：已逃逸前缀的 `production_status()` 在 continuation 销毁前后都报告该失败，而此前已认证的 tile 仍可读取。

### SDK 获取辅助函数

C++ SDK 头文件 `photospider/plugin/tensor_helpers.hpp`、`tile_helpers.hpp` 和 `whole_stage.hpp` 提供非阻塞辅助函数。每个辅助函数要么返回一个 Need 供回调从 poll 中返回，要么返回就绪值；它们都不会执行 producer、等待、填充缺失样本或隐式收集稠密字节。

| 辅助函数 | 就绪值 | 用途 |
| --- | --- | --- |
| `sdk::to_whole(phase, input)` | `WholeTensorInputs`，每个成员一个完整 capability | 获取一个输入的全部成员。 |
| `sdk::to_whole(phase, input, slot)` | 覆盖该成员的 `WholeTensorWindows` | 以拥有型读窗口获取一个完整成员。 |
| `sdk::to_whole(phase, input, slot, samples)` | 按 `samples` 每个 box 裁剪的 `WholeTensorWindows` | 获取成员 ROI。`samples` 为空时返回空的就绪向量且不授予任何内容。 |
| `sdk::to_tile(phase, source, member)` | 覆盖 query tile 的零拷贝 view 组成的 Result | 从拥有的 Result 或 stage Result 发布所选输出 tile。 |
| `sdk::collect(phase, input, region)` | `TensorSpan` | 显式稠密复制一个已授权矩形。 |
| `sdk::publish_dense_tensor(...)` | 通过 `ResultBuilder` 发布 | 为一个矩形显式分配稠密输出。 |

`to_whole` 的 ROI 重载在入口拒绝已取消的 query，并在每个被选样本都获得授权之前返回紧凑的 tile Need。完整获取不意味着稠密收集；稠密复制是算子的显式决定。

`sdk::with_whole_stage(definition, work_bound)` 把一个算法需要完整输出的单输出 CPU 算子适配为一个 Run 共享 stage。当 stage 被请求时，原 factory 在该 Run 内至多运行一次，具有完整输出 schema 且没有所选 tile；它仍负责自己的非阻塞输入 Need、数值策略和资源上限。随后输出 tile 回调通过 `sdk::shared_result_tile` 发布 stage Result 的保留 view。这是算子作者选择的 SDK 适配器，不是内核执行模式。有多个独立输出的算子使用各自的 stage 和更窄的输入投影；输出需联合计算的算子（如稳定排序）使用一个联合 stage。

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

每个 payload 依赖也观察 descriptor 变化。`TileEvidence` 是一次 stage 提交的不可变依赖证据：它拥有该提交的局部 Need，并引用更早的冻结证据。它从不是读取 capability。Stage tile 的消费方只保留对该节点的一个引用。执行表把每个不同节点导入目标 Root 一次并保留其身份，为每个节点只建立一次反向边索引，并从一个加锁的表状态枚举 workflow 依赖。节点按引用计数退役。这些规则避免把 P 个 stage tile 的证据复制到 Q 个消费方中的每一个。

### Mandatory 副作用与重复输入

普通执行运行每个 mandatory 共享 stage，包括 plan 的输出选择为空的情况。显式 `DemandQuery` 在至少一个选择非空时运行 mandatory stage，在全部选择为空时跳过它们。Mandatory stage 在收集具名输出之前达到终态。有副作用的工作不跨 Run 共享或缓存。Mandatory stage 失败或取消会使 Run 失败。

Result C ABI operation 定义可以声明一个固定输入前缀和一个重复的尾部组。`input_count` 与 `inputs` 只描述前缀；`repeated_input` 提供原型约束，在元数据解析和 `start`/`poll` query 校验时应用于每个实际尾部输入。非零 `repeated_maximum` 启用该组，并要求 `1 <= repeated_minimum <= repeated_maximum`，前缀加最大值不超过 1,024 个输入。禁用时，所有重复字段为零或 null。`repeated_match` 为 0 或 1：取值 1 要求每个重复 tensor 与第一个重复 tensor 的 dtype 和完整逻辑 `sample_shape()` 一致；取值 0 要求为展开后的输入列表提供纯元数据解析器。C++ registry 保留未展开的模板，并在准备期间解析实际输入数。该声明只约束元数据；算子仍要为它读取的每个实际输入请求 tile Need。

### Native GPU 输入访问

Native GPU 回调可以对 payload 已授权的 tensor Need 调用 `ResultProgramPhase::acquire_native_atlas(input, slot)`。它返回一个不可变 `FragmentAtlas`，恰好包含该 Need 的覆盖；如何 dispatch 与发布仍由算子决定。一次 poll 内对同一端口和 slot 的调用复用同一个 atlas。CPU 访问、缺失 Need 或 descriptor-only Need 会产生 sticky `InvalidArgument`，且不扩大授权。Root 支持的 payload 与目录存储独立于算子 workspace，atlas 拥有这些缓冲区，但不保留来源 Result 或读窗口。调用方持有的共享 atlas 可以在 poll、来源与执行上下文结束后继续存活。

Native GPU 输入传输仅当弱证明仍标识同一个存活的 CPU 存储且完整 view key 匹配时，才在 Run 内复用原始物理 backing。可选的按字节有界上下文缓存可以按设备实例/构建以及逻辑输入位、Region、dtype 和形状跨 Run 复用上传；它不包含 Result schema，命中后重新绑定到当前 schema。缓存哈希消耗协调器共享的缓存 work 预算，可选 work 耗尽时跳过缓存路径。容量回收先清除可重传的 Run 局部条目，再逐出保留的上下文条目。`host_access_count` 在一个回调范围内对每个存储只记录一次存活 native 支持存储的实际 CPU 暴露；元数据检查、native 地址获取、view 转发和哈希不计数。Native GPU 服务失败在服务调用返回时锁存。之后的 `consume_work` 耗尽或回调状态不能替换这个更早的服务错误；若 work 耗尽先发生，之后的服务错误也不会替换它。

Result atlas 打包接受 `Tensor` 或 `ResultTensorInput` capability。底层的 `FragmentAtlasPlan` 可以在不读取来源样本的情况下打包覆盖为空的 `Tensor`。Result 重载通过 Need 授权的拥有型窗口读取，因此无需稠密转换即可处理有符号与 broadcast stride、碎片化覆盖和带 batch 前缀的 spatial tensor。`preparation_work()` 与 `materialization_work()` 报告通用的几何与打包单位。Result 准备通过 `FootprintLimits::consume_work` 预扣其通用 work；Result 物化还通过该回调预扣窗口获取与读取上限 work。`materialization_work()` 不是 Result 重载的完整扣费。

### 跨输出 block 状态共享

当当前请求与证明匹配时，block 状态保留可以复用已完成的状态。保留状态是执行实现细节；缓存命中数与 block 转换不是公开的性能保证。

纯确定性 Result 算子可以选择启用 `share_blocks_across_outputs`；对不同时满足确定性与无副作用的算子，注册拒绝该选项。C Result ABI 6 以 `PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V6`（标志位 5）提供相同选项。

共享命名空间包括算子及其所有解析后的输出契约、后端、执行模式、所选资源身份、静态参数和实际输入元数据。每个 block key 还包括其 kind、range、mode、输入状态的 schema/覆盖/内容，以及当前提供的 tensor 元数据、覆盖和字节。所选输出索引与输出特定元数据不会单独附加。因此由不同输出共享的计算必须与输出无关，或把差异编码在状态或 mode 中。公开输出 Result、tile 记录、checkpoint 和 completed-result 缓存仍按输出划分且彼此独立。

共享 stage 不提供 checkpoint 或 block 回调。对普通 actor，block 状态查找与保留要求纯、确定且可缓存的算子、纯且确定的所选输入 producer 闭包、无 fallback 污染且没有直接 shared Need。处理 shared Need 后，该 actor 的可选 block 复用被禁用。保留是可选的，计入有界缓存 work 与 Root 容量。若查找或保留不可用，回调正常计算该 block。若查找命中但缓存状态因容量不足无法解包，则视为 block 未命中：协调器释放候选及其 key 租约并正常计算。解包期间的 work 上限、取消和协议失败仍是致命错误。每个输出仍完成自己的 Need 并记录自己的 `TileRecord`。该标志不对并发 producer 去重，也不保证每个 Run 只计算一次。

### CPU fallback

当一次 GPU 尝试符合 CPU fallback 条件时，协调器为重试提供新的失败锁存，丢弃该尝试的 continuation、tensor 与对象 capability、descriptor 输入、已记录 Need和 completed-cache 重放日志，并在 CPU 上重新启动同一计算单元。已完成的 producer 单元保留其已发布的 Result 与记录，因此重试会再次获得它们而无需重算。已消耗的 Root work 与 stage 不退还。重试的单元带 fallback 污染；当提供给消费方的 producer 单元或 stage 提交带污染，或运行在计划外的后端时，消费方也被污染。被污染的单元跳过 checkpoint 查找与发布、block 状态复用和 completed-result 保留。

### Checkpoint

Checkpoint witness 保留实际准入的成员 tile/descriptor Need、producer TileRecord、冻结的继承 stage 证据及 ancestry。恢复 witness 提供依赖证明；callback 通过新发起的授权 Need 重新获取当前输入 Result owner 与窗口。

纯确定性 Result 程序的普通 actor 可以使用 C++ `ResultProgramPhase::checkpoint_publish` 与 `checkpoint_before` 回调保留 continuation 状态；不同时具备两项纯度 trait 的算子调用任一回调都是协议失败。共享 stage 不提供这些回调。查找与保留还要求所选输入的 producer 闭包纯且确定、actor 未处理直接 shared Need 且未被 fallback 污染，并有剩余的可选缓存 work。否则查找未命中、发布被拒绝，但不使算子失败。已发布的 checkpoint 拥有一个已封存的状态 Result 和一个不含 payload 的 witness：该单元至今已准入 Need 的 `TileRecord`、其 producer 闭包已记录的 `TileRecord`、按不同节点各计一次的继承共享证据。

Checkpoint 范围属于一个计算单元：其 key 是 actor key 加 `/checkpoint` 后缀，因此绑定 structured template、冻结 snapshot 身份、step 与输出索引、成员 slot、tile 索引和 tile extent。`checkpoint_before(phase, before)` 返回该 phase 中不大于 `before` 的最大已发布序号。恢复要求同一计算单元。协调器先在可选分配栅栏内暂存 witness Need、producer 记录、继承证据。只有暂存成功后，它才恢复祖先关系，并以同一 Root 内的移动提交暂存值。暂存期间的容量拒绝返回空 checkpoint，且不改变 actor 状态与读取授权。Work 上限、取消和协议失败仍是错误。成功恢复会把 witness Need 合并进当前单元的 `TileRecord`（本次执行中已存在的记录仍具权威）。恢复不会填充当前 phase 的 `results` 或 `tensors`，不会重建读窗口，也不授予 Need 授权。回调必须再次请求当前输入访问。

上下文目录持有 checkpoint 范围的弱引用，活动执行持有其范围的强引用；`ExecutionContextConfig::maximum_result_checkpoint_scopes` 限制存活范围数。每个范围至多保留 64 个条目，一个范围的 witness 总权重不超过依赖 `maximum_boxes` 上限；最旧条目先退役。运行时在构造查找 key、遍历 producer 闭包或复制 Need 记录之前先扣除可选 checkpoint work。可选缓存 work 为零或不足时跳过查找或拒绝发布；不会复制未准入的 witness 数据。复制得到的 `ResultCheckpoint` 在索引或上下文清除后仍拥有其状态与 witness。C Result ABI 通过 poll 作用域的不透明句柄和已封存的通用状态 Result 提供 `checkpoint_before`、`checkpoint_read` 与 `checkpoint_publish`；见 [C checkpoint 服务](Dependency-Data.zh.md#c-checkpoint-服务)契约。

Checkpoint 资格取决于纯度，与 completed-result 缓存无关；block 保留还要求算子可缓存。二者仍受上述 actor 限制。声明 `deterministic = false` 的算子（例如其输出包含 Run 局部 ObjectId）会把每个依赖它的消费方排除在 checkpoint 查找与保留以及 block 缓存准入之外。不可缓存但确定的算子保留自身的 checkpoint 资格。

### Completed-result 缓存

Completed-cache manifest 依赖实际 TileRecord/Need 闭包、不可变 shared-evidence ancestry、相对契约的 producer/source route、已触及 workflow-input tile 内容和 Need 重放日志来证明复用资格。协调器将记录重新绑定到当前 plan、重算来源内容摘要，并检查每个重放 Need 所存的 supplied-facts 摘要，才以当前 owner 和授权接纳缓存的 Result。

当某计算单元实际触及的 workflow 输入内容不变时，completed Result 缓存在冻结执行身份之间（包括绑定 generation 和等价 plan 的不同 Run）复用该单元的已封存 Result。Completed-cache key 摘要 structured template、所选输出索引、计划后端、tile extent、成员 slot 与 tile 索引。Key 不含 step 索引或 graph ID，因此另一 step 上相同的 template 可以复用条目。每个 key 至多保留八个候选。保留的条目（manifest）包含：

- 已封存的 Result；
- 不含 payload 的 tile 快照：该单元的 `TileRecord`、其 producer 闭包已记录的 `TileRecord`、它继承的共享证据与来源路由，以及以契约术语表示的从每个 producer 到输入端口的路由；
- 经该闭包触及的 workflow 输入来源，每个来源是带成员 tile 集合的具名绑定，以及其当前内容的摘要；
- 重放日志：该单元每个已准入的 Need，配以该 Need 之后所提供事实的摘要（规范 schema 与已授权 tensor footprint，不含样本字节）；
- 后端、冻结的 fallback 来源与缓存 epoch。

来源摘要对每个已记录来源覆盖绑定的规范 schema 和被触及成员 tile 的 tensor 样本。没有被任何已记录 Need 触及的字节不影响证明。

查找按契约把每个候选的 tile 快照与共享证据重新绑定到当前 plan。若一个已记录 producer 会映射到不同的当前 producer step，或上下文重排改变了证据路由，则候选未命中，而不会对任意 step 发出 Need。协调器在当前绑定上重新计算来源摘要并与存储的摘要比较。然后通过普通供给路径（包括输入校验器）重放每个已保存 Need，并要求每个已提供事实摘要匹配。通过的候选被采用：协调器以新 ObjectId、当前执行范围重新绑定缓存的 Result，把重新绑定的 `TileRecord` 合并进该单元，并像计算所得输出一样校验发布。缓存条目保留其引用的物理 backing owner；重新绑定不会让旧 Result 包装或旧执行范围成为新输出的身份。

结构化 Result 条目共享有界缓存 LRU 与托管 Root 容量。LRU 至多持有 4,096 个条目，`ExecutionContextConfig::result_cache_bytes` 限制保留的 backing（零表示禁用保留），`maximum_dependency_cache_metadata` 限制 manifest 元数据。容量计算对物理 backing owner 去重（包括通过 tensor view 和 affine owner 触及的 backing），而不是累加逻辑 tensor 大小。缓存候选只暂存计费增量；若候选会在当前额度之外保留旧输入 owner，则作为未命中处理，不改变实时账本。逐出会释放缓存持有的 owner 引用；持有 Result 或读窗口的调用方继续拥有其 backing。`clear()` 推进缓存 epoch，因此上一 epoch 中进行中的 producer 不能重新填充已清空的缓存。缓存证明构造、Need 重放与重新绑定消耗 Run 的可选缓存 work（`ExecutionOptions::maximum_dependency_cache_work`）。可选 work 或 Root 容量不足时跳过保留或产生未命中，不改变本应成功的计算。

每个 Run 拥有的 `StructuredResultCache` 构造 completed-result key 与 manifest、构建来源证明、恢复重放快照，并消耗可选缓存 work 配额。它在每次同步调用中接收借用的上下文与 actor 视图。Run 局部 Host 通过协调器供给 Need，并在重放成功后采用候选；重放失败时恢复 actor 先前的输入与 Need 记录。查找一旦获得强引用候选，就在重放与采用过程中一直持有该 owner；采用不需要再次证明其仍驻留在 LRU 中。可选的提升查找仅当同一 manifest 仍驻留时才把条目移到 LRU 尾部。缓存关闭或 epoch 变化会拒绝采用，而候选 owner 保持存活直到该尝试结束。

保留要求纯且可缓存的算子、纯且可缓存的所选输入 producer 闭包、未被 fallback 污染且在计划后端上运行的单元，以及剩余的可选缓存 work。重放日志超出元数据上限的单元不被保留。每个条目恰好覆盖一个计算单元，不同 tile 的条目不合并。共享 stage 和处理直接 shared Need 的普通 actor 绕过 completed-result 缓存。经 `CompleteBundle`、`StablePrefix` 或完整输出校验器安装的输出也绕过 completed-result 保留。C Result ABI 不暴露该缓存；其实现属于 C++ 执行运行时的私有部分。

### Result builder

`ResultBuilder` 拥有发布。Producer 通过 `publish_tensor`、`publish_tensor_view` 或 `publish_tensor_kernel` 写入 tensor 区域。Tensor 区域依据成员的逻辑样本 domain 检查。重叠、越界访问或资源准入失败会以错误使 producer 退役。`ResultBuilder::start()` 接收 query 的 tile extent；非法的 spatial tile extent 在分配前返回 `InvalidArgument`。

发布把物理数组与 backing owner 保存在源自 Result 的存储 owner 中，而发布状态拥有 Root、资源租约与 schema。销毁时先退役数组 backing，再由发布状态释放其 Root 租约。存储 owner 索引避免每次发布都重新扫描此前已发布的 backing；当成员分区允许时，`acquire_tensor` 通过区间查找定位分块，否则使用有界扫描。`acquire_tensor` 在持有发布互斥锁时复制不可变 backing 句柄，然后先解锁再构造拥有型读窗口。`publish_tensor_kernel` 在互斥锁之外准备回调工作，然后重新获取锁，在提交前校验 owner、revision 与 sticky 失败。读取 capability 保留其已授权的 Result 与 backing，但只在捕获的 descriptor 和被请求的 Need 范围内授予读取。

发布校验器在一次同步校验中接收 `PublicationValidationView` 和借用的 `PublicationValidationServices` 接口。协调器拥有取消、work 准入、当前上限和 actor。校验检查发布身份、revision；它不保留服务，也不执行调度、发布、退役或通知。校验完成后，协调器应用返回的状态与退役意图。

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
| 输入 tensor 授权 | 一个 Need envelope 授予某输入端口的成员。 | 输入 schema 的每个成员都被完整授予。 | Workflow 输入：`InvalidArgument`。产出输入：`OperationFailed`。Origin `Domain`。 |
| 输出发布 | 自上次已校验 revision 以来新增的覆盖。 | 已校验覆盖等于每个成员的完整 domain。 | `OperationFailed`，origin `Domain`。 |
| 共享 stage 提交 | 每次新可见的 stage 提交，在供给消费方之前。 | 可见的 stage 覆盖完整。 | `OperationFailed`，origin `Domain`。 |

带 `requires_complete` 的校验器只看到完整覆盖；不完整的 context 返回 `TypeMismatch`。对完整输出校验器，只有在应用被触发的静态 publication scope 之后 producer 的请求是完整的，协调器才准入 work 与可见性。它依据在收集任何输出之前已合并的、针对该 producer 特化输出的请求判断完整性：该输出所有别名的根输出请求的并集，以及在一个消费方 Need envelope 内对同一 producer step 的 payload 授权并集。Workflow 输入、descriptor-only Need 和空 payload Need 不增加完整性，对不同 producer 或不同特化输出的请求不合并。因此覆盖整个 Result 的 `CompleteBundle` 允许只请求一个成员。闸门组装该 scope 并调用校验器一次。校验器从不扩大调用方的请求或 producer 的输入 Need，每个别名或端口仍只获得自己所选的覆盖。仍不完整的请求不启动 producer，也不调用校验器。完整 stage 校验器把可见性推迟到 stage 静态覆盖完整之后；以不完整覆盖结束的 stage 失败。

Completed-result 缓存命中仍会对重放的 Need 运行输入校验器，但不重复计算。因此无效的 workflow 输入会在返回缓存输出之前以 `InvalidArgument` 失败；校验器据以准备的静态参数发生变化时，会产生不同的 plan cache key。

一次 `validate` 调用中的第一个服务失败具有优先权：若读取失败而回调忽略它，该调用仍返回那个读取失败。失败的校验器服务不能发布结果。`prepare` 或 `validate` 抛出的异常变为 typed 失败。`ValidationDefinition::owner` 保留回调代码；host 只在回调对象与已准备的校验器状态之后销毁它，因此调用方释放 registry 句柄后，已准备的算子仍可工作。

`OperationTraits::diagnostics` 列出算子可能发出的报告类型。回调运行前，host 为这些类型初始化一个 Root 拥有的 `DiagnosticAccumulator`。回调按声明序号通过 `ResultProgramPhase::report_diagnostic`（C：`report_diagnostic`）报告一条定长字节记录；`DiagnosticDefinition::validate` 检查它，`merge` 写入一条完整的新记录，不改变任一输入。失败的报告不改变已累积字节。`OperationTiming::reports` 携带拥有所有权的累加器；`find(identity)` 返回合并后的字节，若该类型从未报告则返回 `NotFound`。内核不解释这些字节。数值 work 报告就是这样一种类型，定义在 SDK 头文件 `photospider/ops/numeric/diagnostics.hpp` 中。

算子可以为失败附加不透明的领域代码。`Status(ErrorCode, message, DomainErrorCode, FailureDetail)` 把它存储在 `FailureDetail::domain_code` 中；C 回调在 run 或 validation 服务上使用 `fail_domain(status_code, domain_code)`。内核保留该代码而不解释，第一个记录的领域失败不会被之后的回调或诊断失败替换。

## 限制与失败处理

每个 `ExecutionContext` 创建一个托管 `ResourceBudget`。若未设置 `ExecutionContextConfig::managed_resources`，上下文使用默认构造的 `ResourceLimits`；显式值提供配置的容量。`maximum_live_bytes` 同时限制 Payload 维度。`ExecutionContext::resource_budget()` 为需要计费临时存储的客户端返回一个共享句柄，其租约可以比上下文存活更久。

执行 Root 在配置的资源维度下计费 payload/backing、work、stage、continuation、队列条目、元数据和保留的 owner。上限都是有限的。过大的 payload 或 owner 准入以资源错误失败，而不是返回不完整的 witness。这一托管容量模型覆盖已插桩的资源；它不是 RSS 上限。

输出可以声明 `maximum_output_payload_bytes`，共享 stage 声明自己的上限。Host 在一个 Run 内对一个逻辑输出的各 tile actor 累计新拥有的 payload，每个唯一物理 owner 只计一次。它排除借用的已授权 owner：通过已暴露 Result、已授权 tensor 授予或已提供的 stage Result 仍存活的来源分配。检查只保留来源的弱引用，因此上限不会让输入 payload 保持存活。第一次符合条件的 poll 在进入回调前从 Result Root 分配上限守卫及其共享控制块；Metadata 容量不足时在进入回调前拒绝该 poll。Owner 枚举与比较消耗普通的 Run 与 Root work。超出上限返回带 `CapacityLimit` 的 `ResourceExhausted`。Workspace、元数据与算子资源准入保持独立。检查在 Result 发布时运行，因此可信回调更早的 Root 分配仍被计费，且不会被该上限阻止。

Result 发布错误是 sticky 的。取消与过期执行只在已准入回调返回后才使活动状态退役。失败的追加或 tensor 发布不能被之后成功的回调修复；此前已认证的前缀保持不可变，并可在其捕获的 descriptor 下读取。最后一个拥有型 Result/读窗口释放时，backing随之退役。

行为回归测试随内核构建注册。`tests/unit/kernel` 与 `tests/integration/kernel` 下的注册是可执行覆盖的当前来源。
