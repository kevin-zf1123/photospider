# 结构化 Result 与 tensor slots

英文权威文档：[Global-Results.md](../Global-Results.md)。

## 模块边界与所有权

`ResultRef` 是结构化 tensor data 与图像数据的语义、发布、ownership 和 input/output 路径。Result 包含 typed tensor slots、packed primitive fields，或二者兼有。Tensor slot 可携带图像语义，并使用内部 planar typed backing；这不创建独立的图像 Result 路径。`Value` 是 backing storage，不是 workflow 或 operation 的 input/output 形式。

当前 package 为 0.33.0，WorkflowDocument schema 为 5，OperationTraits version 为 25，Result C operation ABI 为 2，native provider ABI 为 1。这些版本号描述彼此独立的契约。C++ operation interface 使用 `start_result` 与 `ResultContinuation`。Result C operation table 是唯一的 C operation-plugin 接口；consumer 必须针对已安装 package 重建，因为当前 C++ API 与精确声明的 C 结构是契约。

编译器将不可变 schema 复制到 plan。执行协调器拥有 producer 状态并调度 callbacks 和 I/O。`ResultRef` 拥有 schema、单调 descriptor facts、已认证 backing、dependency evidence 和 source-association metadata。复制引用会共享这些所有权。外部引用或 read window 可在 execution context 结束后继续保持所需 backing 存活。Association metadata 只标识已消费输入，不拥有其 payload。

```cpp
struct ResultTensorLayout final {
  bool spatial = false;
  ImagePlaneOrder order = ImagePlaneOrder::Tiled;
  std::uint32_t height_axis = 0, width_axis = 1;
  std::optional<std::uint32_t> channel_axis = 2;
  std::uint64_t row_pitch_bytes = 0;
  std::vector<ImageComponentGroup> groups;
};

struct ResultTensorSpec final {
  ResourceLease metadata_owner;
  ResourceString key;
  ResourceVector<std::uint64_t> batch_axes;
  std::uint32_t atomic_trailing_axes = 0;
  ValueDescriptor descriptor;
  ResultTensorLayout layout;
  std::vector<ValueFacet> facets;
};

struct SchemaTemplate final {
  ResourceString id;
  std::uint32_t version = 1;
  PublishPolicy publication = PublishPolicy::CompleteBundle;
  ResourceVector<ResultFieldSpec> fields;
  ResourceVector<ResultTensorSpec> tensors;
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

片段省略了默认成员值及其他无关 declarations。带有 `result_schema` 的 workflow input 绑定 owning `ResultRef`；execution 将具名 Result outputs 放入 `ExecutionResult::results`。Tensor sample 坐标由 `batch_axes` 与 descriptor cell axes 拼接而成。不存在独立的 image output map。

完整 structured execution 可为 `ExecutionResult::results` 中的具名 Result outputs 计算非安全用途的 `result_digest`：它包含 canonical schemas、field 行数与字节，以及按逻辑顺序排列的已认证 tensor coverage 和样本字节；不包含 object identity、source association 或 physical layout，也不能用于 authentication。Fragment execution 的摘要为空。`ExecutionOptions::maximum_result_digest_samples` 默认 65,536，限制 field elements 与已认证 tensor samples 的合计数；设为零会禁用摘要。扫描前 coordinator 还会检查每个 field row 的窗口上限、剩余 Run-work，以及 tensor coverage 计数是否可表示。完整摘要预估无法满足这些条件时，coordinator 会省略摘要并继续返回 execution 结果。例如，大型已认证广播 view 不必仅为摘要而按逻辑 sample 数遍历。摘要一旦准入，读取仍使用有界 windows 并消耗 Run-work；扫描期间发生 cancellation、stale、Root/resource 或 read failure 时仍会导致 execution 失败。Coordinator 在成功返回前会重新检查 cancellation 和 stale state。

## 核心数据结构与内存布局

`SchemaTemplate` 最多包含 16 个 field 与 tensor slots、最多八个 domain axes，并带有有界语义 metadata。`ResultFieldSpec` 描述 primitive element type、行数规则，以及 rank 0 到 7、extent 为正数的 record shape。行数可为 fixed、从输入推导、引用前序 field rows，或在运行时发现。Fields 保存 packed primitive records，与 tensor sample bytes 分开存储。

`ResultTensorSpec` 描述一个 typed tensor member。Cell descriptor 的 rank 为 1..8，extents 均为正数；`batch_axes` 添加一个正 extent 前缀，合计 sample rank 不超过八。`atomic_trailing_axes` 将 cell 末尾指定数量的轴闭合为一个不可分割的 observation。Schema validation 不要求 dense allocation，也不要求完整 domain 的元素积可表示。`ResultTensorLayout` 记录可选 spatial layout、physical plane order、row pitch 和 channel groups；storage order 与 pitch 不改变 semantic schema identity。Spatial layout 在 cell descriptor 内映射 height、width 和可选 channel axes，与 batch prefix 分开。

Tensor facets 可附加 sample 规则。Semantic Image facet 要求 height 与 width 分别在 cell axes 0 和 1；ColorArray facet 闭合声明的 channel tuple。Tensor-description facet 可描述 CHW 等其他 layout。Layout groups 本身不会要求 peer channels 或 alpha samples。

Tensor slot 上的 sample 授权保留 legacy semantic image facet 和经验证的 `ColorArray` facet 各自的 tuple 契约。对 rank-3 semantic image cell，空间请求会闭合到完整 channel tuple；若 alpha 是一个 channel，也包含 alpha。对 `ColorArray`，闭包包括所声明 tuple 的全部 channels。Tensor-description facets 和 structural layout groups 不会增加 peer-channel 或 alpha demand。没有这两类 tuple facets 的 typed tensor 按 sample 坐标访问，不会推导 peer closure。逻辑 tuple 完整性与 channel 字节是否交错无关；planar storage 可将 channels 放在不同 planes。

`ResultBuilder` 在创建时分配 object id，此时 runtime counts 尚未确定。Semantic key 包含 canonical schema 和带长度的 execution scope。Scope 绑定 operation contract、参数、有序输入和 captured query。物理 page size、文件偏移、descriptor revision、tile 尺寸和资源限制不会改变语义 identity。`ResultRef::capture()` 在一个 revision snapshot 中捕获已认证的 descriptor、field/tensor relations、descriptor basis 和 dependency bundle。Host actor 只发布这个 captured view，避免 observer 将较早 evidence 与较晚 revision 的 prefix 配对。`ResultRef::read_tensor()` 使用该 descriptor，只读取已认证 samples。`ResultTensorInput` 将显式 tensor Need、slot、descriptor 和授权 sample set 提供给 callback；phase 只在单次 poll 内借用，其 owning capabilities 有各自的生命周期。

对于每个成功提供给 actor 的 Result，host 保存一条 Root-accounted history，键为 input port 和 ObjectId，内容含 descriptor revision。该记录只识别已消费来源，不保留 payload，也不授予读取权限。`ResultObjectInputs` 则跨 poll 保留每个 input port 当前的 owning ResultRef，以支持 object-Need 协议；actor 完成或退役时 host 清空该 map。已保存的 `ResultTensorInput` capability 拥有对应 ResultRef 和 captured descriptor；已获取的 read window 拥有该 Need 授权的 backing。物理 tensor view 会独立保留其源 Results、schema、所选 resources 和 backing。

对于启用了 `preserve_output_views` 的 compiled structured CPU Whole operation，coordinator 会在验证 payload-authorized Tensor Need 后、下一次 computation poll 前准备授权数据。它逐个检查授权 box，先证明其 source backing 可表示为兼容的 affine view。若映射不可用，Auto 可以将授权 samples 收集到不可变的 Root-owned private backing，并由 `ResultTensorInput` 保留；设置 `requires_input_views` 时则会在 computation callback 前返回 `ViewUnavailable`。Auto 不会把 work、取消或资源错误转成收集操作。Capability 副本共享已准备 backing 及其 Root metadata lease。private `Value` 仅用作 backing：workflow 输入输出仍是 Results，发布的 view 保留 source Result identity、association 和 resources。Direct `ResultProgramPhase` 不会自动满足 Need 或准备这些 view。C Result output flags 暴露相同策略；compiled CPU Whole coordinator 会执行 view preparation，而直接进入 C callback 不会自动生成该准备。该路径不扩展到 joint 或 GPU view execution。

Coordinator 分别准备各个 box。如果后续 `ResultTensorInput::acquire()` 跨越的 boxes 没有共同的 affine 映射，它会返回原有的、仍受授权约束的 compound window，不复制样本。Completed-result cache replay 会重新验证保存的 Need 及其 facts，但不会再次执行物理 view preparation；cache miss 则沿普通 callback Need 路径重新准备 view，再进入计算。

Result owner 通过持有不可变 shared bundle 的 opaque `ResultEvidenceOwner` 保留 dependency evidence。Data layer 不 include execution-record 实现；bundle 保存 metadata 与 ancestry，不保存 sample payload。Mutable per-Run recorder、immutable query、import/freeze state 分属不同 owner。退休过程迭代遍历 evidence graph，不递归销毁深层依赖链。

Dependency records 独立保存完整 evidence。`bind_result` 绑定 tensor shapes、field counts 和 descriptor domains；`record_object` 导入来源 coverage 与 relations；`append_relation` 记录每个 output observation 的 support；`capture_bundle` 捕获形成的 dependency graph。Host 在发布 Result 前将该 bundle 绑定到 Result，再由 `ResultRef::capture()` 在同一 revision 冻结 descriptor 和 dependency facts。Actor 完成或退役时会清除临时 input-history map，但不会清除已记录的 dependency evidence。Execution 构造 `ExecutionResult` 时会 snapshot 当前 evidence graph。该 snapshot 独立于后续 actor progress，并在 Root capacity 下准入；容量不足会返回资源错误。

发布前，host 从 actor 的 `(input port, ObjectId)` facts 构造有序 association。`ResultRef::retain_association()` 检查 ID 非零、不等于输出自身，并保留此前已声明的所有 ID。检查使用临时排序副本，不改变保存的 port/ObjectId 顺序，也保留不同 input ports 上重复的 ObjectId。Membership 检查计入 Root work 并观察取消；检查失败时既有 association 不变。Association 没有固定的 1024 项上限，受 Root metadata capacity 和 Run-work 准入限制。

动态行数属于结构化 fields。Callback 消费 `ResultObjectNeed` 后，runtime 会在 field access 前将 Field domain 绑定到 captured Result 已认证的行数。导入 dependency bundle 时会保留已观察的最大 Field domain：`bind_result` 只增大记录的行 extent；`append_relation` 按旧和新 output footprint 的实际已认证 prefix domain 选择关系。非空 footprint 的 domain 是其声明 extent；Empty footprint 认证零行，即使 shape `{1}` 只是占位域。函数将两者提升到最大行域并合并已认证 coverage，选择较长已认证 prefix 的 relation 与 descriptor，长度相同时选择新记录。选择依据是实际已认证 domain，而非 `ResultRelation::coverage()`；后者可能包含尚未认证行的 support。导入较短的 captured prefix 不会缩小记录域。零行 Result 合法，且没有 field payload page；descriptor relation 仍可记录 count、basis 或 validation support。内部 tensor backing 要求各 extent 非零，并拒绝 structural image facet。Empty Footprint 表示在非零逻辑 domain 上没有请求 sample；它不会创建零 extent tensor。

## 调度与发布

每个选中的具名 output 都有独立解析的输出契约和 captured query。`ResultProgramQuery::tensor_outputs` 保存 `tensor_slot` 的请求 footprint，`output_index` 标识选中的 output。没有提供 tensor footprint 时表示请求整个 slot domain。Query、output index、静态参数和 tensor slot 参与 producer identity；tile height、tile width 和 page size 是物理选择。 普通 execution 会将每个非 side-effect-free operation 作为强制 root 执行，包括没有具名 output 的 operation。Fragment 和 atom execution 仅对非空 query 执行这些 roots；完全空的 query 会跳过无关副作用 roots。同一 Run 内，副作用 tensor output 的内部 actor identity 使用 slot 0 和完整 coverage，后续请求可以复用 actor。该规则不授权其他 tensor slots；它们的 Needs 与 publication coverage 仍会校验。副作用工作不会跨 Run 共享或缓存。强制 root failure 或 cancellation 会使 Run 失败。

Result C ABI operation definition 可以声明固定 input prefix 和一个重复的尾部 group。`input_count` 和 `inputs` 只描述固定 prefix；`repeated_input` 提供一个 prototype constraint，在 metadata resolution 及 `start`/`poll` query validation 时应用于每个实际尾部 input。非零 `repeated_maximum` 启用该 group，并要求 `1 <= repeated_minimum <= repeated_maximum`，且 prefix 加 maximum 不超过 1,024 个 inputs。禁用时，所有 repeated 字段均为零或 null。`repeated_match` 只能为 0 或 1：值为 1 时，每个重复 tensor 必须与第一个重复 tensor 的 dtype 和完整逻辑 `sample_shape()` 相同；值为 0 时，扩展后的 input list 必须由 pure metadata resolver 解析。C++ registry 保留未展开的 template，并在 preparation 时根据实际 input count 展开。该声明只约束 metadata；operation 对每个实际读取的 input 仍需请求 Tensor Need。

重复输入契约由 `tests/integration/test_result_repeated.cpp` 及其 C fixture 覆盖，包括扩展后的 input constraint、metadata resolution、malformed DSO 拒绝和 typed reads。Package consumer 通过已注册的 installed suite 验证。这些位置描述行为覆盖，不表示当前完整测试矩阵已全部通过。

Operation 启动一个由宿主拥有的 `ResultContinuation`。每次 poll 返回有界 `ResultProgramNeed` 或 `ResultPublication`。Need 可包含 Result-object、tensor 或 I/O requests。Tensor needs 标明 input index、slot、逻辑 samples 和 roles。协调器依据 Result input schema 与已发布 coverage 校验 Need，然后提供 capability 受限的 `ResultTensorInput`。Callback 可先请求 Control samples，在下一次 poll 检查其值，再请求这些值选出的 Data tensor support。返回的 `ResultRelation` 同时记录这两段 support。

Native GPU callback 可对已有 payload-authorized tensor Need 调用 `ResultProgramPhase::acquire_native_atlas(input, slot)`。它返回只包含该 Need coverage 的不可变 `FragmentAtlas`；dispatch 和 publication 仍由 operation 决定。同一 poll 对同一 port 与 slot 的重复调用复用一个 atlas。CPU 调用、缺少 Need 或 Descriptor-only Need 会产生 sticky `InvalidArgument`，不会扩大授权范围。空的 payload Need 仍返回有效 atlas，含一个惰性 payload 字节和两个空目录 slot，不读取 source samples，也不执行 native dispatch。Root-backed payload 与 directory storage 独立于 operation workspace；atlas 自己拥有这些 buffer，不保留 source Result 或 read window。Caller 持有的 shared atlas 可在 poll、source 和 execution context 结束后继续存活。

Native GPU input transport 仅在 weak proof 仍指向同一 live CPU storage 且完整 view key 匹配时，才会在一个 Run 内复用 raw physical backing。可选的 byte-bounded context cache 可按 device instance/build、logical input bits、Region、dtype 和 shape 跨 Run 复用上传结果；它排除 Result facets 与 resources，命中后按当前 metadata 和 association 重新绑定。Cache hashing 使用 coordinator 共享的 cache-work budget；可选 work 不足时跳过 cache 路径。容量回收先清理可重新传输的 Run-local entries，再驱逐 context 保留的 entries。`host_access_count` 记录 callback scope 内对 live native-backed storage 的实际 CPU 暴露，包括 Whole prepared reads，并按 storage 计一次；metadata 检查、native address 获取、view 转发与 hashing 不计入。Native GPU service 错误会在调用返回时立即 latch。之后发生的 `consume_work` exhaustion 或 callback status 不能覆盖较早的 service error；如果 work exhaustion 先发生，较后的 service error 也不会替换它。

Atlas 可从当前 `ValueFragments` view 或 `ResultTensorInput` capability 准备并物化。Result overload 通过 Need 授权的 owning windows 读取，因此可处理 signed stride、broadcast stride、碎片化 coverage 和带 batch prefix 的 spatial tensor，不会把 Result 转成 Value 语义。`preparation_work()` 和 `materialization_work()` 报告通用几何规划和 packing 工作量。Result preparation 会先通过 `FootprintLimits::consume_work` 预扣通用准备工作；Result materialization 还会用该 callback 预扣 window acquisition 和 read-bound 工作。`materialization_work()` 不是 Result overload 的完整计费。

Block-state retention 可在当前请求与 proof 匹配时复用已完成状态。Retention 属于 execution 实现细节；cache-hit 数量与 block transition 不构成对外性能保证。性能结论需要单独的受控 benchmark。

### 跨输出共享 block state

pure、deterministic 的 Atomic Result-v2 operation 可以启用 `share_blocks_across_outputs`。Registry 会检查 output 与 dependency constraints。C Result ABI 2 使用 `PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V2`（flag bit 5）启用同一能力。

共享 namespace 包含 operation 及全部 resolved output contracts、backend、execution mode、static parameters 和实际 input metadata。每个 block key 还包含 kind、range、mode、incoming state 的 schema/coverage/content，以及当前 supplied tensor 的 metadata、coverage 和 bytes。Selected output index 和 output-specific metadata 不会单独追加到 key。因此跨 output 共享的计算必须与 output 无关，或把差异编码进 state 或 mode。Public output Results、各自的 associations 和 dependency evidence、checkpoints 与 completed-result caches 仍按 output 独立。C opt-in 使用既有 operation flag，不改变 Result table layout。

Retention 是可选的，并受有界 cache work 与 Root capacity 计量。Lookup 或 retention 不可用时，callback 正常计算 block。每个 output 仍须完成自身 Needs 并记录自身 evidence。该 flag 不去重并发 producers，也不保证每个 Run 只计算一次。Block-cache 行为属于实现细节，目前没有性能基准结论。

C bridge 也提供跨 output state-sharing opt-in。每个 output 保留自己的 source association 和 dependency evidence。可选 cache work 或 Root capacity 不足时，runtime 可跳过 retention，callback 会各自计算 state。State reuse 与 transition 数量属于实现观察，不构成性能保证。

GPU Result attempt 只有在算子支持 CPU 并显式允许 fallback 时才可能改用 CPU。Start retry 要求 traits 为 deterministic 且 side-effect-free，并且 cancellation、currentness、operation failure 与 host service 检查均通过。Poll retry 还要求 traits 为 deterministic 且 side-effect-free、attempt 可安全重试，并且没有已发布 output、mandatory I/O、checkpoint 或 block callback、field I/O、native dispatch、cancellation、stale plan 或 active stop。报告的状态必须是未限定的 `BackendUnavailable`（`reason=None`，origin 为未指定或 backend，scope 为未指定或 group）。Host 会单独记录 retry veto；后续 typed resource、protocol、callback、observer 或其他不可重试 failure 会阻止重试，但不会覆盖首个报告状态。重试会退休 GPU continuation 与读取能力，并用新的 failure owners 启动 CPU attempt；Root work 和 stage accounting 继续累计。Result C ABI 的 `api.start` 与 `api.poll` 都由 C Result bridge poll callback 调用，并使用同一个 poll-phase fallback gate。其他错误均为终止错误。Result C ABI 使用 `PS_RESULT_FLAG_CPU_FALLBACK_V2` 显式选择该策略，并要求同时具备 CPU 与 GPU capability。普通 C callback 返回由 `ps_result_status_v2` 定义的整数状态码，与 typed Atom failure codes 分开；poll yield 和 view unavailable 结果遵循各自的 service 契约，native GPU service 使用独立的 GPU status code。成功发布 tensor、view、field 或 Result 后，callback 返回的 `BackendUnavailable` 会变为 `OperationFailed`；单独调用 `begin_result` 不算 publication。Root work 和 stage accounting 不退款；fallback-tainted results 不进入可选缓存。已注册的 `core.gpu_fallback_probe` 覆盖启动阶段情形：它拒绝 GPU startup，再由 CPU 按请求 tensor footprint 执行 identity；GPU startup 在输入访问前失败，因此 retry 不上传输入，也不执行 native dispatch。Diagnostics 保留被拒绝的 GPU startup 与 CPU attempt，并将 CPU 记为选中的 backend。`test_result_plugin.cpp::impure_start_fallback` 检查 non-pure GPU startup 返回 `BackendUnavailable` 时不会改由 CPU 重试；它验证 startup eligibility，不是 native dispatch computation 证据。`test_result_plugin.cpp` 中的 Result C plugin 行为入口还覆盖 opt-in 与禁止 fallback 的 operation、无效或不支持的 backend、C `api.start` 或 `api.poll` callback 返回 `BackendUnavailable`、普通及未知 callback 返回码、取消、仅 begin 的 attempt、publication 后失败、sticky 非法 service 调用、native dispatch 后失败，以及每次 attempt 的 start/destroy 计数。Direct `start_result` 测试还验证非法读取后返回 `BackendUnavailable` 或取消时，sticky `InvalidArgument` 优先保留；native-dispatch 用例验证 native work 后不会启动 CPU attempt。用例检查安全回退不会进入 completed-result cache，并检查非法 publication context 不会污染之后的有效 publication；这里仅说明覆盖位置，不表示测试已通过。

Structured C++ Result runner 会在符合回退条件的 GPU attempt 开始前保存 dependency-record 状态。快照保留精确的类型化 Field、Tensor、Descriptor records 和 relations、domain、已导入 identity，以及不可变且不含 payload 的 evidence owners。新表格 wrapper 与 identity lease 计入 Root；复制可变 metadata 会计入 Root 容量和必需 Run work。成功 Need 和 checkpoint facts 可跨 poll 保留，快照本身不会复制 sample payload。若可安全重试的 attempt 在 native dispatch 或 publication 前返回 `BackendUnavailable`，runner 会丢弃该 attempt 的局部 continuation 与 capabilities，恢复快照，重新导入仍需要的 peer evidence，并在 CPU 上重启同一 query。重新导入时会先绑定 producer 自己的 domain，再合并 records，因此恢复后的 field prefix 可在重试中继续扩展。只有被拒绝 attempt 新增的 records 不会进入 CPU 结果；此前完成的输出证据仍然保留。CPU retry 需要已完成的 Whole ancestor 时，会从本地 Result bundle 导入，避免重复计算。这是精确 Result-record 事务，与旧 dependency-session 路径中仅保存 coverage 的 checkpoint 不同。

C++ `tests/fixtures/operation_fixture.cpp` 导出 16 个 Result ABI 2 operations，其中包括 `fixture.bad_bytes`。`test_plugin_registry.cpp` 使用 Result workflow 检查 binding/readback 和 public start validation，也覆盖 dynamic output metadata：resolver 将 input facet 复制到推导出的 output schema，poll callback 随后读取 sample 并乘以 `scale`。`fixture.bad_facet` 用例让 metadata sink 收到 version 为零的 facet 并验证拒绝；`fixture.bad_bytes` 向 8-byte tensor 提供 9-byte payload，预期为 `TypeMismatch`。Fallback mode 5 和 7 尝试通过 tensor slot 99 发布，预期为 `InvalidArgument`。Owner checks 覆盖 owner allocation failure、plugin destroy 和 native-library close 计数。`test_execution.cpp` 覆盖 GPU fallback opt-in、Result callback outcomes、重复 publication、取消和 fallback accounting。Native-only invocation 单独注册；无 GPU lane 时以 code 77 跳过。重复 publication 测试区分有效的重复 publication（`OperationFailed`）与非法的首次 service call：后者在后续有效尝试后仍保留 sticky `InvalidArgument`。实际 host cancellation 可覆盖前者，callback 自行返回 `Cancelled` 则不能覆盖。Result C callback 通过 typed status code 报告结果；测试观察 typed status、timing 和 invocation counts，numeric diagnostics 则通过独立的 `report_numeric` service 提供。C++ operation execution 使用 staged Result-v2 契约。Result ABI 2 C table 是唯一的 C operation-plugin interface；不存在 Base C plugin table。

纯且确定性的 Result program 可以通过 C++ `ResultProgramPhase::checkpoint_publish` 和 `checkpoint_before` callbacks 保存 continuation state。还要求所有被选中的 input producer 构成的完整上游闭包均纯且确定。若包含的 producer 不可共享，lookup 会正常 miss，publication 会正常放弃保留。已发布 checkpoint 拥有一个 sealed Result state，以及成功 Needs 的 witness：input port、target、slot、roles、footprint、已消费的 ObjectIds 和 revisions，以及 producer inputs 的 dependency bundles。Scalar Needs 也会记录。Object Need 会记录 descriptor，以及所有 fields 和 tensors 的已认证 coverage。

Checkpoint lookup 按 operation template、frozen execution snapshot 和选定的 tensor slot 隔离。请求的 output footprint 不参与该范围，因此不同 demand 可以复用 state；template 仍包含选定 output 与其静态契约。`checkpoint_before(phase, before)` 返回同一 phase 中不大于 `before` 的最大已发布 sequence。成功恢复时，也会用恢复后的 input history 刷新当前 poll 的 `association`；此前元素的引用或迭代器可能失效，但借用的 vector 对象会在 poll 返回前保持有效。恢复不会填充当前 phase 的 `results` 或 `tensors`，不会重建 read windows，也不会授予 Need 权限。Callback 必须重新请求当前 input access。

Context directory 使用 checkpoint scopes 的 weak references，活动 execution 则强持有其 scope。每个 scope 最多保留 64 个 entries，并受 Root metadata 与 work 准入限制。Runtime 会在构造 lookup key、遍历 producer closure，或复制 Need history 与 metadata 之前，先计入可选 checkpoint work。捕获 producer dependency bundle 时，也会计入完整 dependency 遍历和复制的 evidence。可选 cache fuel 为零或不足时，可以跳过 lookup 或放弃 publication，不会先复制未准入的 witness 数据。复制出的 `ResultCheckpoint` 即使在 index 或 context 清除后，仍拥有其 state 和 witness。可选 cache、metadata 或 capacity 耗尽可以导致 lookup miss 或拒绝保留 checkpoint，而不使成功 operation 失效；取消、protocol failure 和已准入的 Run-work failure 仍会返回错误。C Result ABI 通过 poll-scoped opaque handles 与 sealed generic state Results 提供 `checkpoint_before`、`checkpoint_read` 和 `checkpoint_publish`；契约见 [C checkpoint 服务](Dependency-Data.zh.md)。

Completed Result cache 会在 consumed source content 未改变时，跨 frozen execution identity（包括不同 generation）复用已 sealed 的 Result output。缓存项持有完整 Result、transitive dependency bundle、typed source observations，以及原先成功的 Result Need 序列和每个 Need 之后观察到的 ready facts。Cache key 包含 operation template、选定 output、tensor slot、backend 和精确的 tensor 请求 footprint `Q`，因此候选项必须使用相同的 `Q`。对每个候选项，runtime 只对记录的 Field 和 Tensor supports 计算当前输入字节摘要，并对 Descriptor supports 计算 descriptor facts 与 schema 摘要。Target、slot、roles 和 footprint 均参与 proof；Data、Control、Validation 和 Descriptor roles 保持彼此独立。随后 runtime 按当前 bindings 重放每个已保存的 Result Need，并要求 ready facts 完全匹配，才接受候选项。未观察的输入字节不会影响此 proof。

Cache hit 会创建具有新 ObjectId 和当前 execution scope 的 Result。新 Result 使用当前有序 input association，并复用 cached Result 的 sealed descriptor、relations 和 tensor backing。缓存项持有其引用的物理 backing owners；rebind 不会让旧 Result wrapper 或旧 execution scope 成为新 output 的 identity。当前 upstream Results 会经由普通 dependency recorder 重放，因此发布到 execution 的 evidence 对应当前 bindings。同一 frozen execution 内由共享 producer 服务的重复请求仍走既有 weak producer sharing 路径，并使用原 ObjectId。

Structured Result entries 与其他缓存内容共用有界 LRU 和 managed Root capacity。Capacity 按实际物理 backing owner 去重计量，也包括通过 tensor views 和 affine owners 持有的 backing，而不是把逻辑 tensor 大小相加。Eviction 释放缓存持有的 owner references；仍被 caller 的 Result 或 read window 持有的 backing 会继续存活。`clear()` 会推进 cache epoch，因此旧 epoch 的在途 producer 不能重新填入已清空的 cache。Cache proof 构造、Need replay 和 rebind 都消耗可选 cache work。可选 work 或 Root capacity 不足会跳过 retention 或产生 cache miss，不会改变本来成功的计算结果。

每个 Run 拥有编译后的 `StructuredResultCache`，负责构造 completed-result keys 和 manifests、生成 source proofs、恢复 replay snapshots，并扣除可选 cache-work quota。每次同步调用都接收借用的 context 与 actor views。Run-local Host 通过 coordinator 提供 Needs，并在 replay 成功后接纳 candidate。Lookup 一旦取得强 candidate owner，就会持有它完成 replay 和 adoption；adoption 不要求再次证明 LRU residency。可选 promotion lookup 只有在同一 manifest 仍驻留时才会将 entry 移至 LRU 尾部。Cache closing 或 epoch 变化会拒绝 adoption，而 candidate owner 会保持有效到该次尝试结束。

Dependency-v2 Result operation 有一种受限的非纯情况：Whole、非 joint、Atomic 且使用 RequestFailureOnly delivery 的 output 可以清除任一 purity trait，但必须不可缓存。同一 Run 中不同但等价 nodes 的合并，以及同一 Frozen snapshot 的不同 Runs 之间的 weak producer/result sharing，都要求 selected-input producer closure 为 pure；两者的 purity gate 均独立于 optional cache-work budget。同一 impure Whole node、slot 的重复非空 Tensor 请求仍会在一个 Run 内复用经 Whole 规范化的 query；不同 impure nodes，以及 selected-input closure 含 impure producer 的 consumers，仍保持隔离。非纯 closure 不参与跨 node 或跨 Run sharing。跨不同 snapshots 或 generations 的 completed-result cache 复用另有更严格的要求：selected-input closure 必须 pure 且 cacheable，并且 optional retention 成功。

Retention 要求 operation 及其 selected-input producer closure 均 pure 且 cacheable，并且没有观察到 backend fallback。`photospider.path_set` 被排除，因为其 fields 内嵌自身 ObjectId。若候选 Result 或其任一 source owner 尚未 complete，也不会进入 cache。当前 cache 要求 tensor footprint 完全相同：不同 `Q` 的 entries 尚不能组合；重建 descendant 当前 dependency evidence 时，也可能重新运行已从 cache eviction 的 Whole producer。C Result ABI 不暴露该 cache；实现位于 C++ execution runtime 内部。

`ResultBuilder` 拥有发布过程。Producer 绑定 descriptor support，通过 `publish_tensor` 写入 tensor regions，并为每次 publication 提供 relation 和 finality。Tensor regions 及其 support 会按 slot 展平后的逻辑 sample domain 校验。重叠、越界访问、不完整 finality 或资源准入失败都会以错误终结 producer。Seal 会检查所有 fields 和 tensor relations 后才完成 Result。

Publication 将物理 arrays 和 backing owners 放在 Result 派生的 storage owner 中；publication state 持有 Root、resource leases、schema、relations 与 evidence。销毁时先退休 arrays/backing，再由 publication state 释放 Root leases。`acquire_tensor` 在 publication mutex 下复制不可变 backing handles，随后解锁并创建 owning read window，不会重新加锁。`publish_tensor_kernel` 在 mutex 外准备 callback 工作，再重新加锁校验 owner、revision 与 sticky failure 后提交。Read capability 会保留授权的 Result 与 backing，但只允许读取 captured descriptor 和当前 Need 授权的范围。

Publication validator 在一次同步 validation 期间接收 `PublicationValidationView` 和借用的 `PublicationValidationServices` interface。Coordinator 拥有 cancellation、work 准入、当前 limits 和 actor。Validation 检查 publication identity、revision、association 与 relation obligations；它不会保留 services，也不会负责调度、发布、retirement 或通知。Validation 完成后由 coordinator 处理返回的 status 和 retirement intent。

每次 tensor publication 都会在 guarantee 非 `Unknown` 时认证 relation，并将所有 relation 限制到该次 publication 的精确 region 后累积。`Unknown` relation 也必须匹配 tensor slot 的完整 output shape。不同 region 保留各自的 support，包括 role 和 target；union 不会填入未发布的间隙，也不会把 `Unknown` guarantee 升级。Restriction 保存紧凑的 region 几何，不会枚举整个 tensor canvas。后续 region 补齐矩形 frontier 时，coalescer 可以重新压缩 union；只有底层 witness 和 output shape 相同、至多一个轴不同且该轴区间重叠或相接时才会合并。分离区间保持独立，不会用 bounding box 代替 union。对于 `Unknown` relation，`visit_declared()` 只在某个 leaf 自己的 mask 内检查该 leaf，因此 sibling region 的 mask 不会隐藏其已声明 witness。此区域规则用于 tensor samples；field 与 descriptor relation 沿用既有契约。

`tests/support/result_diagnostics_fixture.hpp::verify_cpp_fixed_broadcast` 用一个 8-byte zero-stride scalar backing，为 `{4}`、`{UINT64_MAX}` 和 `{UINT64_MAX,2}` 三种 shape 发布 full-coverage Float64 Results。`test_plugin_registry.cpp` 和 `test_result_diagnostics.cpp` 都会调用此 helper。每个 output 的 payload cap 为 8 bytes，因此两个独立 Result 可在 16-byte Root Payload 限额内共存；测试读取逻辑域首尾 sample，并检查释放两个输出后 live Payload 回到零。精确的最大 sample count 报告 `computed_elements == UINT64_MAX` 且 `computed_elements_saturated == false`。额外 tensor case 的两个 tensor member 合计有 `UINT64_MAX + 1` 个逻辑样本，但共享一个 8-byte backing；它仍能发布，并将计数饱和为 `UINT64_MAX`。

`publish_tensor_kernel` 为请求区域借出可写 windows。对于 spatial tensor，完整逻辑 sample 的字节几何可表示时，builder 使用 planar rectangles。如果 layout 非 spatial、sample count 不可表示，或完整 sample count 乘 element width 会溢出，builder 会在 Root limits 内为请求区域分配 affine backing。此回退保留 tensor spec、spatial layout、全局 sample 坐标、relation 和 certified coverage，只改变物理 writer backing。Callback 必须使用每个 window 的 `spec()`、`region()`、`sample_axis()`，以及 `row_run()` 或 `rectangle_run()` 返回的 strides；不能假设 backing 是 planar 或行连续。巨大逻辑 spatial domain 上的 sparse request 按实际可写区域计费；更大的请求区域仍须满足 Root payload 与 work limits。

发布遵循 schema policy。`CompleteBundle` 在 seal 前隐藏输出。`StablePrefix` 和 `IndependentChunks` 只暴露有序且不可撤销的 certified field prefixes/ranges；当前 publisher 不提供任意乱序 chunk publication。Callback 给出的 `ResultFinality` 是算法作者承担的义务，覆盖 data、control、validation 和 descriptor facts；宿主检查声明义务是否完整，但不会证明任意 callback 实际读取了什么。后续 operational failure 不会撤销已认证 prefix，较早取得的 descriptor snapshot 也不会自动获得新发布的 coverage。

发布的 Result 记录有序 source ObjectIds，用于 lineage 与 dependency 报告。Association 是 metadata，不是 input payload 的 strong owner，也不授予读取权限。`ResultObjectInputs`、`ResultTensorInput` capabilities 和 retained windows 提供上文说明的活动 owner 与授权路径。Physical tensor view 也会保留它发布的所有 source Results。Result resources 保留 schema 声明的 owners，包括 typed image facets 引用的 ICC/OCIO owners。Compiler 从 supplied bindings 选择 nested resources，runtime bindings 还会在 execution resource root 下重新准入。Public `ResourceMap` 和 `ResourceVector` containers 也携带 allocator 与 managed ownership；复制的 results、relations、names 和 observations 会在容器及其所含 `ResultRef` owners 释放前持续占用 root 计量的 metadata。Consumer 应将这些复制与 payload、relation 工作一起纳入预算。该策略偏保守，因此仍存活的 owners 可能保留比最终输出本身所需更多的 backing。

## Dependency evidence 与 identity

`ResultRelation` 是不可变、有界的 evidence，将每个展平后的输出 observation 映射到输入 support。Support 地址包含 input index、target（`Value`、`Field`、`Tensor` 或 `Descriptor`）、slot、sample interval 和 roles（`Data`、`Control`、`Validation`、`Descriptor`）。Dirty 匹配会同时检查 roles 和 target。Descriptor observation 独立于 field/tensor sample 坐标；空数据集仍可能依赖 runtime count 或 semantic basis。

### C++ 与 C 的终端 RequestRecord outputs

C++ `ObservationKind::RequestRecord` output 或以 `PS_RESULT_REQUEST_RECORD_V2` 声明的 C ABI 2 output，会对选中 output 的 captured semantic-closure query Q 执行一次。即使 operation region rule 为 Whole，Result executor 也不会把给定 Q 扩展到完整 tensor domain。Continuation 必须发布一个完整 sealed Result，使用选中的 output schema、semantic scope 和 Root；被选中的 tensor slot 必须恰好覆盖 Q。Partial Result publication 和 coverage 不匹配都属于 protocol error。

Dependency recorder 为该请求捕获一个由 Root 计量、不可拆分的 manifest。Manifest 包含已发布 fields 与 tensor slots 的 relations、descriptor witness，以及按 input port、role、target 和 slot 键控的宿主 Descriptor/Validation obligations。对相同 Result 和 request identity 重复 capture 或 import 时，会从旧 manifest 初始化并合并，因此宿主添加的 obligations 会保留。Edit 与任一匹配 support row 和 role 相交时，`potential_dirty` 返回整个 Q。`Unknown` guarantee 不能证明 Q clean，会得到 unresolved 结果。该 manifest 不是 Atomic certificate；`ExecutionDependencies::restrict` 会拒绝 Q 的真子集。

选中的 output 与精确 Q 都参与 request identity，不同 Q 保留独立 manifest。`ExecutionContextConfig::result_cache_bytes` 大于零时，只要 captured proof 匹配，completed-result content cache 可在等价的新 Result sources 或 graph renumbering 间复用相同 Q；cache rebind 会创建新的 Result identity 并保留 terminal marker。禁用 completed-result retention 后，同一次 execution 中相同请求仍可共享活动 producer。`ResultRef::capture`、weak lookup 和 cache rebinding 均保留该 marker。Executor 会在启动下游 consumer 前拒绝将带 marker 的 Result 作为 operation input；tensor-view publication、checkpoint state 和 block state 也会拒绝它。当 incoming/computed state 都是普通非 terminal Result 时，终端 query 内仍可使用纯 `ResultProgramPhase::block` 工作。C ABI 2 为每个具名 output 声明 `observation_kind` 和 `failure_delivery`。其可选 CPU `joint` callback 支持 contract 1 和 2：contract 1 使用 RequestFailureOnly delivery，contract 2 使用 PerAtomOutcome。Singleton callbacks 仍然必需并返回普通 status code；typed Atom failure 和 quality attachment 可通过 contract-2 joint outcome 提供。

符合条件的 C++ terminal `RequestRecord`、C `PS_RESULT_REQUEST_RECORD_V2` 或 contract-2 joint output，在 tensor Q 为空且 output 不含 fields 时使用宿主 stateless empty-result 路径。静态参数和 schema validation、backend 选择及 operation resource admission 仍会执行。Host 检查 failure 与 cancellation 后，直接 seal 一个 tensor coverage 为空的 Result，不调用 producer callbacks，也不读取输入。此路径不会创建 AtomObservation 或 validation-ledger finality。普通 Atomic Empty request 仍会调用 producer，因为它可能需要读取 Control 或 Validation 输入；含 fields 的 output 也会运行 producer。

对于 structured Result consumer 发出的 Tensor Need，如果 computed contract-2 producer 的请求样本并集为空且其 output schema 不含 fields，coordinator 也使用普通 Empty Result 路径。静态 schema 检查、backend 选择和 resource admission 仍会执行。Descriptor-only grant 和 coverage 为空的 payload-role grant 都适用。Host 提供 sealed metadata Result，不调用 producer callback、不创建 AtomObservation，也不 finalize atom domain。Consumer 只收到 `ResultTensorInput`；该 grant 不会填充 `phase.results`，不授权 Object Need，也不允许读取 sample。含 fields 的 computed C2 Empty tensor Need 不由此 shortcut 支持。后续非空 Tensor Need 仍会展开为实际 atom queries。

`test_result_request_record.cpp` 覆盖 Whole region rule 下的精确 Q、不同具名 output aliases 的独立请求、descriptor 与 typed support、whole-Q dirty 和拒绝子集 restrict、未解析的 `Unknown`、fresh-source 与 graph-renumber cache rebind、Empty tensor query 处理、terminal marker 拒绝，以及纯 block state 与 terminal Result 的区分。`test_result_atoms.cpp` 覆盖跳过 scalar-bound 输入的 Empty contract-2 output、不会创建 atom 或 domain finality 的 Empty sibling query，以及 Descriptor-only 和 Empty payload grant 的 `metadata_tensor_need` 用例。这些用例检查 Empty tensor capability 和拒绝 sample 读取；同一次 execution 中还会先发 Empty metadata Need，再发针对 point 的 Data Need，producer 报告的 `ValidationDomain/InvalidDomain` 失败会作为 consumer outcome 保留，而不会被 protocol error 取代。另有对同一 frozen producer 的独立请求，确认 producer 报告的失败仍为 `ValidationDomain/InvalidDomain`。这些是行为覆盖入口，本段不声称 GPU 能力。

`test_result_atoms.cpp` 中的 `field_collector` 覆盖 collection-only C2 Field output。Source 先响应 Object Need；callback 再通过 I/O 读取一行 field；下游 C2 member 请求该 Result object 并继续读取 field。`execute_atoms` 和普通 `execute` 都会在 joint grouping 启用、禁用两种配置下运行此流程。检查包括输出值 `7`、source Field support 和对应的 dirty projection。这是单行 collection，不代表通用的宽 Object 或 Field 聚合。

`compound_profile_owners` 使用 shape `{65,4}` 的 65 个 observations 和 `atomic_trailing_axes=1`，因此每个 observation 都闭合其四个 channel samples。每个 source Result 都有一行关联 Field，并为内容相同的 profile 使用各自独立的 `CpuStorage` backing；tensor samples 则共享符合现有 affine identity-view 映射条件的 backing。Consumer 获得 `object_id()==0` 且包含 65 个 association IDs 的 compound `ResultTensorInput`，并对该 backing 发布 view。即使释放 input capability、source window、execution context 和编译期 profile handles，只要输出及其已加载 window 仍存活，原始 Results、field pages 和 profile storage 就继续存活。Fixture 从每个 source field 读取 `1000+at`，再从最终 tensor window 读取 `260`；随后释放 owners，并检查两个 Root 的 live usage 均归零。启用 joint grouping 时产生两个 callback groups；禁用时产生 65 个。运行配置 Host/Metadata 各 4 MiB。Payload cap 为 `(65 或 1)*profile_bytes + 2,080 + 8,192`；更低 cap 或 10 million Run-work limit 会在中途失败并释放已准入 owners，成功路径使用 100 million work allowance。这些是有界 ownership 检查，不是吞吐量或固定的单图内存结论；它们不证明 ICC identity 不同、执行了 CMM 或支持 GPU。

`ResultRelation::prefix(budget, count, input, roles, target, slot)` 表示 rank-one domain `[0,count)` 上精确的 inclusive-prefix dependency，其中 `1 <= count <= UINT64_MAX`。输出 `j` 依赖输入 `[0,j+1)`。Relation 只保存固定节点与两个长度为一的 shape vectors，metadata 大小不随 domain 长度增加。Role mask 可组合 Data、Control 和 Validation；Descriptor role 或 target 无效。访问单个输出只报告一个 support span。将请求输出 `Q` 投影时，至多产生一个展平 support span `[0,max_end(Q))`；Empty 不产生访问。逆向投影将最早变更输入 `k` 映射到 `Q ∩ [k,count)`。Projection 与 inverse 的 work、取消及 metadata capacity 均受限，并返回类型化失败状态。

`ResultRelation::neighborhood(budget, shape, radii, periodic, support)` 是另一种 compact exact node。它接受相同的 tensor 坐标域，rank 为 1..8，每个 uint64 extent 为正，并且每个轴有一个 uint64 radius；sample 总数的完整乘积可以超过 uint64。Relation 保存各轴 radius，不建立逐样本表。每个输出样本的 support 是各轴对称的矩形邻域，并按 periodic 选择裁剪到 tensor domain 或逐轴环绕；所有 radius 为零时即 identity support。Forward projection 扩展请求矩形，inverse projection 返回邻域与变更输入相交的请求输出。较大的矩形投影可表示 sample 总数乘积溢出的 domain；枚举单个 sample 的 support 仍可能耗尽 work limit。Shape、radius 和矩形 metadata、work、box 数量、取消与容量均受 execution root 和 query limits 约束。失败返回类型化 status，不会把 support 扩宽为 bounding box。

C Result service `make_prefix` 只在相同 rank-one 输入/输出 tensor slots 之间创建此 relation。成功时返回供 `publish_tensor_with_relation` 使用的 relation handle，并由 `release_relation` 退役。创建 relation 不授予 payload 读取权限；callback 仍须单独通过 `need_tensor` 请求输入数据。此 C relation helper 与数值算子实现相互独立：`numeric.ordered_scan` 使用 C++ Result phase 的 prefix relation 和 checkpoint services；分阶段 `numeric.mean` 与 `numeric.variance` 使用其 block-state service。

C Result service `make_neighborhood` 在所选 output 与 input tensor slots 的完整 sample shape 相同时创建相同的 relation。借用的 radii array 每轴一个 uint64 值；periodic 必须为 0（裁剪）或 1（逐轴环绕）。Roles 是 Data、Control 和 Validation 的非空组合。`CResultMemberBridge` 会跨 poll 保留返回的 relation handle，直到调用 `release_relation` 或 continuation 销毁；该 handle 可传给 `publish_tensor_with_relation`。Poll-scoped service table 和 context 仅在当前 poll 内借用，因此服务调用必须发生在该 poll 的 entry thread。创建 relation 不授权读取输入；服务失败仍是 sticky poll error。

`Exact` 表示对已注册 operation 契约而言，声明的 support 完整。它不表示输入值变化必然改变输出位。`Conservative` 允许更宽的 support。`Unknown` 表示 unresolved，不能据此证明输出 clean。Dirty query 将 captured relation 与变更 samples 比较，返回可能受影响的范围，不比较数值是否相等。

Semantic schema identity 包含 typed slots 和语义 metadata，但排除物理 page/tile geometry、storage offsets、object ids 和 descriptor revisions。会影响调度的物理选择进入 execution plan。Result keys 还包含 operation contract、参数、有序输入 identity 和 captured query。改变需要重新编译的静态 schema 或 descriptor facts 时，必须使用新 plan。

## 限制与错误处理

每个 `ExecutionContext` 都会创建 managed `ResourceBudget`。若未设置 `ExecutionContextConfig::managed_resources`，context 使用默认构造的 `ResourceLimits`；显式值用于配置容量。`maximum_live_bytes` 也会限制 Payload 维度。`ExecutionContext::resource_budget()` 返回供临时存储客户端共享的 root handle，其 leases 可以比 context 存活更久。

Execution root 按各自配置资源维度计量 payload/backing、work、stages、I/O、relation/map 构造、continuations、queue entries、metadata 和保留的 owners。各类资源都有有限上限。Relation traversal、payload 或 owner 准入超限时返回资源错误，不返回不完整 witness。该 managed-capacity 模型计量已 instrument 的资源，不构成 RSS 上限。

Output 可声明 `maximum_output_payload_bytes`。对于 structured Result operation，host 在发布时按每个 output 的 cap 检查 Result 的实际物理 backing，对共享 owner 去重，并排除仍由已暴露 Result 或已授权 tensor grant 保活的 source allocations，其中包括由 `ResultTensorInput` 保留的 Whole 私有 input backing。检查只保存 source 的 weak references，因此 cap 不会延长输入 payload 的生命周期。Root 仍会计费 source 和收集后的 input backing；output cap 只衡量新增的 output payload。每次 prefix publication 都检查完整 Result，因此随着 backing 增加，检查覆盖累计结果。Direct singleton、compiled execution 和 contract-2 joint 的每个 member 均采用相同规则。首次 eligible poll 会在进入 callback 前从 Result Root 分配 cap guard 及其 shared control block；Metadata 容量不足会在 callback 执行前拒绝该 poll。Owner 遍历与比较会消耗通常的 Run/Root work。超过 cap 返回带 `CapacityLimit` 的 `ResourceExhausted`；contract-2 将其作为该 member 的局部失败，扫描成员期间发生的取消也只取消该 member。Workspace、metadata 和 ICC/OCIO referenced-resource 准入分别计量。检查发生在 Result publication 时，因此 trusted callback 在发布前已经进行的 Root allocations 仍照常计费，cap 不会阻止这些分配。

Result publication errors 具有 sticky 语义。取消和 stale execution 会在已准入 callbacks 返回后终结活动状态。只要还有其他 waiter，某一个 waiter 的取消就不会取消共享 producer。Append 或 tensor publication 失败后，后续成功 callback 不能将其修复为成功；先前已认证的 prefix 仍可通过 captured descriptor 读取。最后一个 owning Result/read-window 被释放时，backing 和保留的 associations 才退休。

内核构建与 installed consumer suite 注册行为回归测试。`tests/integration` 和 `tests/consumer` 中的注册项是当前可执行覆盖范围；本规格说明运行时契约，不维护历史测试清单。
