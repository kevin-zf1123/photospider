# 结构化 Result 与图像 slots

英文权威文档：[Global-Results.md](../Global-Results.md)。

## 模块边界与所有权

`ResultRef` 是图像数据唯一的语义、发布、ownership 和 input/output 路径。Result 可以包含 typed image slots、packed primitive fields，或二者兼有。`PlanarImage` 是 image slot 内部的标准存储 backing；它不提供并列的图像结果或执行路径。其创建、导入、views、读写和发布均为 Result owners 的 private 操作。普通非图像 numeric `Value` 仍可使用独立的合法存储形式。

当前 package version 为 0.30.0，workflow schema 为 4，semantic operation traits 为 version 21。Numeric C operation table 仍为 ABI 11；Result operation table 为 ABI 1。这些版本号描述彼此独立的契约。

编译器将不可变 schema 复制到 plan。执行协调器拥有 producer 状态并调度 callbacks 和 I/O。`ResultRef` 拥有 schema、单调 descriptor facts、已认证 backing、dependency relations 和保留的输入 Result owners。复制引用会共享这些所有权。外部引用或 read window 可在 execution context 结束后继续保持所需 backing 存活。

```cpp
struct ResultImageSpec final {
  ResourceString key;
  std::uint64_t frames = 1, layers = 1;
  ValueDescriptor descriptor;
  PlanarImageLayout layout;
  std::vector<ValueFacet> facets;
};

struct SchemaTemplate final {
  ResourceString id;
  std::uint32_t version = 1;
  PublishPolicy publication = PublishPolicy::CompleteBundle;
  ResourceVector<ResultFieldSpec> fields;
  ResourceVector<ResultImageSpec> images;
  ResourceVector<ResultExtent> domain;
  ResourceVector<ResultFacet> metadata;
};

struct WorkflowInputDeclaration final {
  std::uint64_t id;
  std::string name;
  std::shared_ptr<const SchemaTemplate> result_schema;
};

struct ExecutionBinding final {
  std::string name;
  ResultRef result;
};
```

片段省略了普通 Value declarations、默认成员值及其他 binding alternatives。带有 `result_schema` 的 workflow input 绑定 owning `ResultRef`；execution 将具名 Result outputs 放入 `ExecutionResult::results`。不存在独立的 image output map。

## 核心数据结构与内存布局

`SchemaTemplate` 最多包含 16 个 field 与 image slots、最多八个 domain axes，并带有有界语义 metadata。`ResultFieldSpec` 描述 primitive element type、行数规则，以及 rank 0 到 7、extent 为正数的 record shape。行数可为 fixed、从输入推导、引用前序 field rows，或在运行时发现。Fields 保存 packed primitive records，不是图像存储；图像字节不会编码成 primitive field records。`ResultImageSpec` 有自己的 key、descriptor、layout、facets 和 frame/layer identity，并为每个 frame/layer pair 拥有 planar backing。

图像 slot 的逻辑坐标为 `{frame, layer, descriptor axes...}`。Descriptor 保留已声明的轴顺序；`PlanarImageLayout` 将 height、width 和可选 channel axes 映射到这些坐标。例如，shape `{6, 2, 4}` 可将 channel 映射到 axis 0、height 映射到 axis 1、width 映射到 axis 2。`frames` 和 `layers` 均为正数，二者乘积最多为 4096 个 backing pairs。图像 descriptor 接受 rank 2 或 3；frame 和 layer 保持独立轴。Facets 可增加轴约束。Semantic Image facet 要求 height 与 width 分别在 axes 0 和 1；ColorArray facet 将 channel tuple 放在 descriptor 最后一个轴。只有 TDM facet 的 slot 可使用其他映射，例如 CHW。Layout groups 本身不会要求 peer channels 或 alpha samples。

Sample 授权保留 legacy semantic image facet 和经过验证的 `ColorArray` facet 各自现有的 tuple 契约。对 rank-3 semantic image，空间请求会闭合到完整 channel tuple；若 alpha 是一个 channel，也包含 alpha。对 `ColorArray`，闭包包括所声明 tuple 的全部 channels。只有 TDM 的 facets 和 structural layout groups 不会增加 peer-channel 或 alpha demand。没有这两类 tuple facets 的 typed slot 按 descriptor sample 坐标访问，不会推导出 peer closure。逻辑 tuple 完整性与 channel 字节是否交错无关；planar storage 可将 channels 放在不同 planes。

`ResultBuilder` 在创建时分配 object id，此时 runtime counts 尚未确定。Semantic key 包含 canonical schema 和带长度的 execution scope。Scope 绑定 operation contract、参数、有序输入和 captured query。物理 page size、文件偏移、descriptor revision、tile 尺寸和资源限制不会改变语义 identity。`ResultRef::capture()` 在一个 revision snapshot 已认证的 descriptor、field/image relations、descriptor basis 和 dependency bundle。Host actor 只发布这个 captured view，避免 observer 将较早 evidence 与较晚 revision 的 prefix 配对。Result 的有序 source association 是独立的 live state：它保留已消费的 source owners，并可随 producer 消费更多 inputs 而单调扩展。Captured descriptor 不会获得后续授权。`ResultRef::read_image()` 必须使用该 descriptor，且只能读取已认证 samples。`ResultImageInput` 将显式 image Need、slot、descriptor 和获准的 sample set 传给 callback；该 poll 返回后，借用的 phase 及其中的 views 失效。

动态行数属于结构化 fields。Callback 消费 `ResultObjectNeed` 后，runtime 会在 field access 前将 Field domain 绑定到 captured Result 实际认证的行数。零行 Result 合法，且没有 field payload page；descriptor relation 仍记录 count、basis 或 validation support。普通 `Value` 存储要求 extent 非零，并拒绝 structural image facet。只要各自的 descriptor/facet contracts 允许，Numeric、Signal 和 LUT Values 仍使用普通 Value 存储。空的 Value 请求 Footprint 可以表示在非零 Value domain 上不请求任何 sample；它不会创建零 extent Value。

## 调度与发布

每个选中的具名 output 都有独立解析的输出契约和 captured query。`ResultProgramQuery::image_outputs` 保存所选 image slot 的请求 footprint，`output_index` 标识选中的 output。没有提供 image footprint 时表示请求整个 slot domain。Query、output index、静态参数和 image slot 参与 producer identity；tile height、tile width 和 page size 是物理选择。

Operation 启动一个由宿主拥有的 `ResultContinuation`。每次 poll 返回有界 `ResultProgramNeed` 或 publication。Image input needs 标明 input index、slot、逻辑 samples 和 roles。协调器依据 Result input schema 与已发布 coverage 校验 Need，然后提供 capability 受限的 `ResultImageInput`。Callback 可先请求 Control samples，在下一次 poll 检查其值，再请求这些值选出的 Data image support。返回的 `ResultRelation` 同时记录这两段 support。

`ResultBuilder` 拥有发布过程。Producer 绑定 descriptor support，通过 `publish_image` 写入 image regions，并为每次 publication 提供 relation 和 finality。Image regions 及其 support 会按 slot 展平后的逻辑 sample domain 校验。重叠、越界访问、不完整 finality 或资源准入失败都会以错误终结 producer。Seal 会检查所有 fields 和 image relations 后才完成 Result。

发布遵循 schema policy。`CompleteBundle` 在 seal 前隐藏输出。`StablePrefix` 和 `IndependentChunks` 只暴露有序且不可撤销的 certified field prefixes/ranges；当前 publisher 不提供任意乱序 chunk publication。Callback 给出的 `ResultFinality` 是算法作者承担的义务，覆盖 data、control、validation 和 descriptor facts；宿主检查声明义务是否完整，但不会证明任意 callback 实际读取了什么。后续 operational failure 不会撤销已认证 prefix，较早取得的 descriptor snapshot 也不会自动获得新发布的 coverage。

Callback 消费输入 Results 时，发布的 Result 会保留有序输入 object association 及其 strong owners。随着继续消费输入，association 可以单调扩展；已经声明的 object 不能移除。这样可保持后续读取和 support 查询所需的 ancestry 存活。Result resources 保留 schema 声明的 owners，包括 typed image facets 引用的 ICC/OCIO owners。Compiler 从 supplied bindings 选择 nested resources，runtime bindings 还会在 execution resource root 下重新准入。Public `ResourceMap` 和 `ResourceVector` containers 也携带 allocator 与 managed ownership；复制的 results、relations、names 和 observations 会在容器及其所含 `ResultRef` owners 释放前持续占用 root 计量的 metadata。Consumer 应将这些复制与 payload、relation 工作一起纳入预算。该策略偏保守，因此仍存活的 owners 可能保留比最终输出本身所需更多的 backing。

## Dependency evidence 与 identity

`ResultRelation` 是不可变、有界的 evidence，将每个展平后的输出 observation 映射到输入 support。Support 地址包含 input index、target（`Value`、`Field`、`Image` 或 `Descriptor`）、slot、sample interval 和 roles（`Data`、`Control`、`Validation`、`Descriptor`）。Dirty 匹配会同时检查 roles 和 target。Descriptor observation 独立于 field/image sample 坐标；空数据集仍可能依赖 runtime count 或 semantic basis。

`Exact` 表示对已注册 operation 契约而言，声明的 support 完整。它不表示输入值变化必然改变输出位。`Conservative` 允许更宽的 support。`Unknown` 表示 unresolved，不能据此证明输出 clean。Dirty query 将 captured relation 与变更 samples 比较，返回可能受影响的范围，不比较数值是否相等。

Semantic schema identity 包含 typed slots 和语义 metadata，但排除物理 page/tile geometry、storage offsets、object ids 和 descriptor revisions。会影响调度的物理选择进入 execution plan。Result keys 还包含 operation contract、参数、有序输入 identity 和 captured query。改变需要重新编译的静态 schema 或 descriptor facts 时，必须使用新 plan。

## 限制与错误处理

Execution root 按各自配置资源维度计量 payload/backing、work、stages、I/O、relation/map 构造、continuations、queue entries、metadata 和保留的 owners。各类资源都有有限上限。Relation traversal、payload 或 owner 准入超限时返回资源错误，不返回不完整 witness。该 managed-capacity 模型不构成 RSS 上限。

Result publication errors 具有 sticky 语义。取消和 stale execution 会在已准入 callbacks 返回后终结活动状态。只要还有其他 waiter，某一个 waiter 的取消就不会取消共享 producer。Append 或 image publication 失败后，后续成功 callback 不能将其修复为成功；先前已认证的 prefix 仍可通过 captured descriptor 读取。最后一个 owning Result/read-window 被释放时，backing 和保留的 associations 才退休。

13 项 focused 检查全部通过，包括 `test_result_execution` 的 8192 行分页、prefix、取消和共享行为，以及 `test_result_image_contracts` 对不可变 captured facts、relation guarantees、owner retirement、跨 frame support、tuple closure 和 semantic-alias diamond rebind 的覆盖。`test_execution_dependencies`、`test_multi_output_execution` 和 `test_generic_result_cache` 也通过，验证现有 numeric workflows、alias root-cache 命中（实际结果 14）、正确 dirty 传播和有限 proof work。Native Metal 测试通过 affine/broadcast packed transfer，并在 500-unit 限额下验证 root-work 拒绝。C11 Result fixture 与 native Result fixture 通过；后者回读到浮点值 4。五个 installed consumers 均通过：unified workflow、C++、Result contracts、C11 和 native GPU。C 覆盖还验证了从已消费 `ResultObjectNeed` 行数绑定 runtime Field domain，以及 Descriptor/Field 在零行与非零行之间替换。`source_support()` 预算不足时返回带类型的 ResourceExhausted 结果。
