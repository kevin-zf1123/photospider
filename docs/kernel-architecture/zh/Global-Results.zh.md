# 结构化 Result 与 tensor slots

英文权威文档：[Global-Results.md](../Global-Results.md)。

## 模块边界与所有权

`ResultRef` 是结构化 tensor data 与图像数据的发布、ownership 和 input/output 路径。Result 包含 typed tensor slots、packed primitive fields，或二者兼有。Tensor slot 可声明 spatial layout，并使用内部 planar typed backing；这不创建独立的图像 Result 路径。`Value` 是 backing storage，不是 workflow 或 operation 的 input/output 形式。

Result 只携带结构、数据以及存储所需的 owner。它没有附着的领域描述、semantic kind 或 facet 列表；内核不会把颜色、alpha、transfer 或采样描述从输入传播到输出，也不会在连线上匹配这类标签。结构相同的两个 Result 可以连接到同一个端口，即使调用方赋予它们不同含义。需要领域解释的算子以显式静态参数接收该解释，并可通过绑定的校验器证明样本合法性（见[端口校验器与诊断报告](#端口校验器与诊断报告)）。

当前 package 为 0.34.0，WorkflowDocument schema 为 6，OperationTraits version 为 26，Result C operation ABI 为 4。这些版本号描述彼此独立的契约。C++ operation interface 使用 `start_result` 与 `ResultContinuation`。Result C operation table 是唯一的 C operation-plugin 接口；consumer 必须针对已安装 package 重建，因为当前 C++ API 与精确声明的 C 结构是契约。[ADR 0022](../../adr/zh/0022-freeze-operation-c-abi-v3.zh.md)（Proposed）记录所有 operation 共用的按前缀协商 C ABI；package 0.34.0 尚未实现。

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
};

struct SchemaTemplate final {
  ResourceString id;
  std::uint32_t version = 1;
  PublishPolicy publication = PublishPolicy::CompleteBundle;
  ResourceVector<ResultFieldSpec> fields;
  ResourceVector<ResultTensorSpec> tensors;
  ResourceVector<ResultExtent> domain;
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

片段省略了默认成员值及其他无关 declarations。带有 `result_schema` 的 workflow input 绑定 owning `ResultRef`；execution 将具名 Result outputs 放入 `ExecutionResult::results`。Tensor sample 坐标由 `batch_axes` 与 descriptor cell axes 拼接而成。不存在独立的 image output map。`ResultTensorSpec::metadata_owner` 是为该 spec 的 managed 嵌套容器付费的资源租约，属于预算所有权，不是附着的解释。`id` 与 `version` 命名显式结构契约并参与编译身份；内核不会据此分派到某种领域含义。

完整 structured execution 可为 `ExecutionResult::results` 中的具名 Result outputs 计算非安全用途的 `result_digest`：它包含 canonical schemas、field 行数与字节，以及按逻辑顺序排列的已认证 tensor coverage 和样本字节；不包含 object identity、source association 或 physical layout，也不能用于 authentication。Fragment execution 的摘要为空。`ExecutionOptions::maximum_result_digest_samples` 默认 65,536，限制 field elements 与已认证 tensor samples 的合计数；设为零会禁用摘要。扫描前 coordinator 还会检查每个 field row 的窗口上限、剩余 Run-work，以及 tensor coverage 计数是否可表示。完整摘要预估无法满足这些条件时，coordinator 会省略摘要并继续返回 execution 结果。例如，大型已认证广播 view 不必仅为摘要而按逻辑 sample 数遍历。摘要一旦准入，读取仍使用有界 windows 并消耗 Run-work；扫描期间发生 cancellation、stale、Root/resource 或 read failure 时仍会导致 execution 失败。Coordinator 在成功返回前会重新检查 cancellation 和 stale state。

## 核心数据结构与内存布局

`SchemaTemplate` 最多包含 16 个 field 与 tensor slots，以及最多八个 domain axes。`ResultFieldSpec` 描述 primitive element type、行数规则，以及 rank 0 到 7、extent 为正数的 record shape。行数可为 fixed、从输入推导、引用前序 field rows，或在运行时发现。Fields 保存 packed primitive records，与 tensor sample bytes 分开存储。

`ResultTensorSpec` 描述一个 typed tensor member。Cell descriptor 的 rank 为 1..8，extents 均为正数；`batch_axes` 添加一个正 extent 前缀，合计 sample rank 不超过八。`atomic_trailing_axes` 将 cell 末尾指定数量的轴闭合为一个不可分割的 observation。Schema validation 不要求 dense allocation，也不要求完整 domain 的元素积可表示。`ResultTensorLayout` 记录可选 spatial layout、physical plane order、row pitch 和 channel groups；storage order 与 pitch 不改变 semantic schema identity。Spatial layout 在 cell descriptor 内映射 height、width 和可选 channel axes，与 batch prefix 分开。Spatial tensor 可以有任意合法通道数；内核不会从通道数或 layout groups 推断 RGB、alpha、颜色模型或数值范围。

`atomic_trailing_axes` 是唯一的 tuple 完整性规则。Sample 授权把每个请求闭合到这些末尾轴上，因此触及某个 observation 中一个 sample 的请求会得到完整 observation。对 `atomic_trailing_axes = 1` 的 HWC 图像，空间请求包含每个请求像素的全部通道；若 alpha 存储在该轴上，也包含 alpha。Layout groups、schema id 和 spatial 标志不会增加 peer-channel 或 alpha demand。`atomic_trailing_axes = 0` 的 tensor 严格按 sample 坐标访问。逻辑 tuple 完整性与 channel 字节是否交错无关；planar storage 可将 channels 放在不同 planes。[ADR 0022](../../adr/zh/0022-freeze-operation-c-abi-v3.zh.md) D9 的规范图像目标使用显式逻辑 axis 角色，不强制 HWC。

`ResultBuilder` 在创建时分配 object id，此时 runtime counts 尚未确定。Semantic key 包含 canonical schema 和带长度的 execution scope。Scope 绑定 operation contract、参数、有序输入和 captured query。物理 page size、文件偏移、descriptor revision、tile 尺寸和资源限制不会改变语义 identity。`ResultRef::capture()` 在一个 revision snapshot 中捕获已认证的 descriptor。`ResultRef::read_tensor()` 使用该 descriptor，只读取已认证 samples。`ResultTensorInput` 将显式 tensor Need、slot、descriptor 和授权 sample set 提供给 callback；phase 只在单次 poll 内借用，其 owning capabilities 有各自的生命周期。

对于每个成功提供给 actor 的 Result，host 保存一条 Root-accounted history，键为 input port 和 ObjectId，内容含 descriptor revision。该记录只识别已消费来源，不保留 payload，也不授予读取权限。`ResultObjectInputs` 则跨 poll 保留每个 input port 当前的 owning ResultRef；actor 完成或退役时 host 清空该 map。已保存的 `ResultTensorInput` capability 拥有对应 ResultRef 和 captured descriptor；已获取的 read window 拥有该 Need 授权的 backing。物理 tensor view 会独立保留其源 Results、schema 和 backing。

对于启用了 `preserve_output_views` 的 compiled structured CPU Whole operation，coordinator 会在验证 payload-authorized Tensor Need 后、下一次 computation poll 前准备授权数据。它逐个检查授权 box，先证明其 source backing 可表示为兼容的 affine view。若映射不可用，Auto 可以将授权 samples 收集到不可变的 Root-owned private backing，并由 `ResultTensorInput` 保留；设置 `requires_input_views` 时则会在 computation callback 前返回 `ViewUnavailable`。Auto 不会把 work、取消或资源错误转成收集操作。Capability 副本共享已准备 backing 及其 Root metadata lease。private `Value` 仅用作 backing：workflow 输入输出仍是 Results，发布的 view 保留 source Result identity 和 association。Direct `ResultProgramPhase` 不会自动满足 Need 或准备这些 view。C Result output flags 暴露相同策略；compiled CPU Whole coordinator 会执行 view preparation，而直接进入 C callback 不会自动生成该准备。该路径不扩展到 GPU view execution。

Coordinator 分别准备各个 box。如果后续 `ResultTensorInput::acquire()` 跨越的 boxes 没有共同的 affine 映射，它会返回原有的、仍受授权约束的 compound window，不复制样本。Completed-result cache replay 会重新验证保存的 Need 及其 facts，但不会再次执行物理 view preparation；cache miss 则沿普通 callback Need 路径重新准备 view，再进入计算。

发布前，host 从 actor 的 `(input port, ObjectId)` facts 构造有序 association。`ResultRef::retain_association()` 检查 ID 非零、不等于输出自身，并保留此前已声明的所有 ID。检查使用临时排序副本，不改变保存的 port/ObjectId 顺序，也保留不同 input ports 上重复的 ObjectId。Membership 检查计入 Root work 并观察取消；检查失败时既有 association 不变。Association 没有固定的 1024 项上限，受 Root metadata capacity 和 Run-work 准入限制。

## 调度与发布

每个选中的具名 output 都有独立解析的输出契约和 captured query。`ResultProgramQuery::tensor_outputs` 保存 `tensor_slot` 的请求 footprint，`output_index` 标识选中的 output。没有提供 tensor footprint 时表示请求整个 slot domain。Query、output index、静态参数和 tensor slot 参与 producer identity；tile height、tile width 和 page size 是物理选择。 普通 execution 会将每个非 side-effect-free operation 作为强制 root 执行，包括没有具名 output 的 operation。Fragment 和 atom execution 仅对非空 query 执行这些 roots；完全空的 query 会跳过无关副作用 roots。同一 Run 内，副作用 tensor output 的内部 actor identity 使用 slot 0 和完整 coverage，后续请求可以复用 actor。该规则不授权其他 tensor slots；它们的 Needs 与 publication coverage 仍会校验。副作用工作不会跨 Run 共享或缓存。强制 root failure 或 cancellation 会使 Run 失败。

Result C ABI operation definition 可以声明固定 input prefix 和一个重复的尾部 group。`input_count` 和 `inputs` 只描述固定 prefix；`repeated_input` 提供一个 prototype constraint，在 metadata resolution 及 `start`/`poll` query validation 时应用于每个实际尾部 input。非零 `repeated_maximum` 启用该 group，并要求 `1 <= repeated_minimum <= repeated_maximum`，且 prefix 加 maximum 不超过 1,024 个 inputs。禁用时，所有 repeated 字段均为零或 null。`repeated_match` 只能为 0 或 1：值为 1 时，每个重复 tensor 必须与第一个重复 tensor 的 dtype 和完整逻辑 `sample_shape()` 相同；值为 0 时，扩展后的 input list 必须由 pure metadata resolver 解析。C++ registry 保留未展开的 template，并在 preparation 时根据实际 input count 展开。该声明只约束 metadata；operation 对每个实际读取的 input 仍需请求 Tensor Need。

重复输入契约由 `tests/integration/kernel/test_result_repeated.cpp` 及其 C fixture 覆盖，包括扩展后的 input constraint、metadata resolution、malformed DSO 拒绝和 typed reads。这些位置描述行为覆盖，不表示当前完整测试矩阵已全部通过。

Native GPU callback 可对已有 payload-authorized tensor Need 调用 `ResultProgramPhase::acquire_native_atlas(input, slot)`。它返回只包含该 Need coverage 的不可变 `FragmentAtlas`；dispatch 和 publication 仍由 operation 决定。同一 poll 对同一 port 与 slot 的重复调用复用一个 atlas。CPU 调用、缺少 Need 或 Descriptor-only Need 会产生 sticky `InvalidArgument`，不会扩大授权范围。空的 payload Need 仍返回有效 atlas，含一个惰性 payload 字节和两个空目录 slot，不读取 source samples，也不执行 native dispatch。Root-backed payload 与 directory storage 独立于 operation workspace；atlas 自己拥有这些 buffer，不保留 source Result 或 read window。Caller 持有的 shared atlas 可在 poll、source 和 execution context 结束后继续存活。

Native GPU input transport 仅在 weak proof 仍指向同一 live CPU storage 且完整 view key 匹配时，才会在一个 Run 内复用 raw physical backing。可选的 byte-bounded context cache 可按 device instance/build、logical input bits、Region、dtype 和 shape 跨 Run 复用上传结果；它排除 Result schema 与 association，命中后按当前 schema 和 association 重新绑定。Cache hashing 使用 coordinator 共享的 cache-work budget；可选 work 不足时跳过 cache 路径。容量回收先清理可重新传输的 Run-local entries，再驱逐 context 保留的 entries。`host_access_count` 记录 callback scope 内对 live native-backed storage 的实际 CPU 暴露，包括 Whole prepared reads，并按 storage 计一次；metadata 检查、native address 获取、view 转发与 hashing 不计入。Native GPU service 错误会在调用返回时立即 latch。之后发生的 `consume_work` exhaustion 或 callback status 不能覆盖较早的 service error；如果 work exhaustion 先发生，较后的 service error 也不会替换它。

Atlas 可从当前 `ValueFragments` view 或 `ResultTensorInput` capability 准备并物化。Result overload 通过 Need 授权的 owning windows 读取，因此可处理 signed stride、broadcast stride、碎片化 coverage 和带 batch prefix 的 spatial tensor，不会把 Result 转成 Value 语义。`preparation_work()` 和 `materialization_work()` 报告通用几何规划和 packing 工作量。Result preparation 会先通过 `FootprintLimits::consume_work` 预扣通用准备工作；Result materialization 还会用该 callback 预扣 window acquisition 和 read-bound 工作。`materialization_work()` 不是 Result overload 的完整计费。

Block-state retention 可在当前请求与 proof 匹配时复用已完成状态。Retention 属于 execution 实现细节；cache-hit 数量与 block transition 不构成对外性能保证。性能结论需要单独的受控 benchmark。

### 跨输出共享 block state

pure、deterministic 的 Atomic Result-v2 operation 可以启用 `share_blocks_across_outputs`。Registry 会检查 output 与 dependency constraints。C Result ABI 4 使用 `PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V4`（flag bit 5）启用同一能力。

共享 namespace 包含 operation 及全部 resolved output contracts、backend、execution mode、static parameters 和实际 input metadata。每个 block key 还包含 kind、range、mode、incoming state 的 schema/coverage/content，以及当前 supplied tensor 的 metadata、coverage 和 bytes。Selected output index 和 output-specific metadata 不会单独追加到 key。因此跨 output 共享的计算必须与 output 无关，或把差异编码进 state 或 mode。Public output Results、各自的 associations 和 dependency evidence、checkpoints 与 completed-result caches 仍按 output 独立。C opt-in 使用既有 operation flag，不改变 Result table layout。

Retention 是可选的，并受有界 cache work 与 Root capacity 计量。Lookup 或 retention 不可用时，callback 正常计算 block。每个 output 仍须完成自身 Needs 并记录自身 evidence。该 flag 不去重并发 producers，也不保证每个 Run 只计算一次。Block-cache 行为属于实现细节，目前没有性能基准结论。

C bridge 也提供跨 output state-sharing opt-in。每个 output 保留自己的 source association 和 dependency evidence。可选 cache work 或 Root capacity 不足时，runtime 可跳过 retention，callback 会各自计算 state。State reuse 与 transition 数量属于实现观察，不构成性能保证。

Structured C++ Result runner 会在符合回退条件的 GPU attempt 开始前保存 dependency-record 状态。快照保留精确的类型化 Field、Tensor、Descriptor recordss、domain、已导入 identity，以及不可变且不含 payload 的 evidence owners。新表格 wrapper 与 identity lease 计入 Root；复制可变 metadata 会计入 Root 容量和必需 Run work。成功 Need 和 checkpoint facts 可跨 poll 保留，快照本身不会复制 sample payload。若可安全重试的 attempt 在 native dispatch 或 publication 前返回 `BackendUnavailable`，runner 会丢弃该 attempt 的局部 continuation 与 capabilities，恢复快照，重新导入仍需要的 peer evidence，并在 CPU 上重启同一 query。重新导入时会先绑定 producer 自己的 domain，再合并 records，因此恢复后的 field prefix 可在重试中继续扩展。只有被拒绝 attempt 新增的 records 不会进入 CPU 结果；此前完成的输出证据仍然保留。CPU retry 需要已完成的 Whole ancestor 时，会从本地 Result bundle 导入，避免重复计算。这是精确 Result-record 事务，与旧 dependency-session 路径中仅保存 coverage 的 checkpoint 不同。

C++ `tests/fixtures/operation_fixture.cpp` 导出 16 个 Result ABI 4 operations，其中包括 `fixture.bad_bytes`。`test_plugin_registry.cpp` 使用 Result workflow 检查 binding/readback 和 public start validation，也覆盖 dynamic output metadata：resolver 将 input schema 复制到推导出的 output，poll callback 随后检查 input 的 atomic trailing axis，读取 sample 并乘以 `scale`。`fixture.bad_atomic_axis` 用例让 metadata sink 收到 `atomic_trailing_axes = 2` 的 rank-1 tensor 并验证拒绝；`fixture.bad_bytes` 向 8-byte tensor 提供 9-byte payload，预期为 `TypeMismatch`。Fallback mode 5 和 7 尝试通过 tensor slot 99 发布，预期为 `InvalidArgument`。Owner checks 覆盖 owner allocation failure、plugin destroy 和 native-library close 计数。`test_execution.cpp` 覆盖 GPU fallback opt-in、Result callback outcomes、重复 publication、取消和 fallback accounting。Native-only invocation 单独注册；无 GPU lane 时以 code 77 跳过。重复 publication 测试区分有效的重复 publication（`OperationFailed`）与非法的首次 service call：后者在后续有效尝试后仍保留 sticky `InvalidArgument`。实际 host cancellation 可覆盖前者，callback 自行返回 `Cancelled` 则不能覆盖。Result C callback 通过 typed status code 报告结果；测试观察 typed status、timing 和 invocation counts，有界报告则通过独立的 `report_diagnostic` service 提供。C++ operation execution 使用 staged Result-v2 契约。Result ABI 4 C table 是唯一的 C operation-plugin interface。

纯且确定性的 Result program 可以通过 C++ `ResultProgramPhase::checkpoint_publish` 和 `checkpoint_before` callbacks 保存 continuation state。还要求所有被选中的 input producer 构成的完整上游闭包均纯且确定。若包含的 producer 不可共享，lookup 会正常 miss，publication 会正常放弃保留。已发布 checkpoint 拥有一个 sealed Result state，以及成功 Needs 的 witness：input port、target、slot、roles、footprint、已消费的 ObjectIds 和 revisions，以及 producer inputs 的 dependency bundles。Scalar Needs 也会记录。Object Need 会记录 descriptor，以及所有 fields 和 tensors 的已认证 coverage。

Checkpoint lookup 按 operation template、frozen execution snapshot 和选定的 tensor slot 隔离。请求的 output footprint 不参与该范围，因此不同 demand 可以复用 state；template 仍包含选定 output 与其静态契约。`checkpoint_before(phase, before)` 返回同一 phase 中不大于 `before` 的最大已发布 sequence。成功恢复时，也会用恢复后的 input history 刷新当前 poll 的 `association`；此前元素的引用或迭代器可能失效，但借用的 vector 对象会在 poll 返回前保持有效。恢复不会填充当前 phase 的 `results` 或 `tensors`，不会重建 read windows，也不会授予 Need 权限。Callback 必须重新请求当前 input access。

Context directory 使用 checkpoint scopes 的 weak references，活动 execution 则强持有其 scope。每个 scope 最多保留 64 个 entries，并受 Root metadata 与 work 准入限制。Runtime 会在构造 lookup key、遍历 producer closure，或复制 Need history 与 metadata 之前，先计入可选 checkpoint work。捕获 producer dependency bundle 时，也会计入完整 dependency 遍历和复制的 evidence。可选 cache fuel 为零或不足时，可以跳过 lookup 或放弃 publication，不会先复制未准入的 witness 数据。复制出的 `ResultCheckpoint` 即使在 index 或 context 清除后，仍拥有其 state 和 witness。可选 cache、metadata 或 capacity 耗尽可以导致 lookup miss 或拒绝保留 checkpoint，而不使成功 operation 失效；取消、protocol failure 和已准入的 Run-work failure 仍会返回错误。C Result ABI 通过 poll-scoped opaque handles 与 sealed generic state Results 提供 `checkpoint_before`、`checkpoint_read` 和 `checkpoint_publish`；契约见 [C checkpoint 服务](Dependency-Data.zh.md)。

Completed Result cache 会在 consumed source content 未改变时，跨 frozen execution identity（包括不同 generation）复用已 sealed 的 Result output。缓存项持有完整 Result、transitive dependency bundle、typed source observations，以及原先成功的 Result Need 序列和每个 Need 之后观察到的 ready facts。Cache key 包含 operation template、选定 output、tensor slot、backend 和精确的 tensor 请求 footprint `Q`，因此候选项必须使用相同的 `Q`。对每个候选项，runtime 只对记录的 Field 和 Tensor supports 计算当前输入字节摘要，并对 Descriptor supports 计算 descriptor facts 与 schema 摘要。Target、slot、roles 和 footprint 均参与 proof；Data、Control、Validation 和 Descriptor roles 保持彼此独立。随后 runtime 按当前 bindings 重放每个已保存的 Result Need，并要求 ready facts 完全匹配，才接受候选项。未观察的输入字节不会影响此 proof。

Cache hit 会创建具有新 ObjectId 和当前 execution scope 的 Result。新 Result 使用当前有序 input association，并复用 cached Result 的 sealed descriptor 和 tensor backing。缓存项持有其引用的物理 backing owners；rebind 不会让旧 Result wrapper 或旧 execution scope 成为新 output 的 identity。当前 upstream Results 会经由普通 dependency recorder 重放，因此发布到 execution 的 evidence 对应当前 bindings。

Structured Result entries 与其他缓存内容共用有界 LRU 和 managed Root capacity。Capacity 按实际物理 backing owner 去重计量，也包括通过 tensor views 和 affine owners 持有的 backing，而不是把逻辑 tensor 大小相加。Eviction 释放缓存持有的 owner references；仍被 caller 的 Result 或 read window 持有的 backing 会继续存活。`clear()` 会推进 cache epoch，因此旧 epoch 的在途 producer 不能重新填入已清空的 cache。Cache proof 构造、Need replay 和 rebind 都消耗可选 cache work。可选 work 或 Root capacity 不足会跳过 retention 或产生 cache miss，不会改变本来成功的计算结果。

每个 Run 拥有编译后的 `StructuredResultCache`，负责构造 completed-result keys 和 manifests、生成 source proofs、恢复 replay snapshots，并扣除可选 cache-work quota。每次同步调用都接收借用的 context 与 actor views。Run-local Host 通过 coordinator 提供 Needs，并在 replay 成功后接纳 candidate。Lookup 一旦取得强 candidate owner，就会持有它完成 replay 和 adoption；adoption 不要求再次证明 LRU residency。可选 promotion lookup 只有在同一 manifest 仍驻留时才会将 entry 移至 LRU 尾部。Cache closing 或 epoch 变化会拒绝 adoption，而 candidate owner 会保持有效到该次尝试结束。

Dependency-v2 Result operation 有一种受限的非纯情况：Whole、Atomic 且使用 RequestFailureOnly delivery 的 output 可以清除任一 purity trait，但必须不可缓存。同一 Run 中不同但等价 nodes 的合并要求 selected-input producer closure 为 pure；purity gate独立于 optional cache-work budget。同一 impure Whole node、slot 的重复非空 Tensor 请求仍会在一个 Run 内复用经 Whole 规范化的 query；不同 impure nodes，以及 selected-input closure 含 impure producer 的 consumers，仍保持隔离。非纯 closure 不参与跨 node sharing。跨不同 snapshots 或 generations 的 completed-result cache 复用另有更严格的要求：selected-input closure 必须 pure 且 cacheable，并且 optional retention 成功。

Retention 要求 operation 及其 selected-input producer closure 均 pure 且 cacheable，并且没有观察到 backend fallback。若候选 Result 或其任一 source owner 尚未 complete，也不会进入 cache。当前 cache 要求 tensor footprint 完全相同：不同 `Q` 的 entries 尚不能组合；重建 descendant 当前 dependency evidence 时，也可能重新运行已从 cache eviction 的 Whole producer。C Result ABI 不暴露该 cache；实现位于 C++ execution runtime 内部。

`ResultBuilder` 拥有发布过程。Producer 通过 `publish_tensor` 写入 tensor regions。Tensor regions 会按 slot 展平后的逻辑 sample domain 校验。重叠、越界访问、资源准入失败都会以错误终结 producer。

Publication 将物理 arrays 和 backing owners 放在 Result 派生的 storage owner 中；publication state 持有 Root、resource leases、schema。销毁时先退休 arrays/backing，再由 publication state 释放 Root leases。`acquire_tensor` 在 publication mutex 下复制不可变 backing handles，随后解锁并创建 owning read window，不会重新加锁。`publish_tensor_kernel` 在 mutex 外准备 callback 工作，再重新加锁校验 owner、revision 与 sticky failure 后提交。Read capability 会保留授权的 Result 与 backing，但只允许读取 captured descriptor 和当前 Need 授权的范围。

Publication validator 在一次同步 validation 期间接收 `PublicationValidationView` 和借用的 `PublicationValidationServices` interface。Coordinator 拥有 cancellation、work 准入、当前 limits 和 actor。Validation 检查 publication identity、revision、association；它不会保留 services，也不会负责调度、发布、retirement 或通知。Validation 完成后由 coordinator 处理返回的 status 和 retirement intent。

`tests/support/result_diagnostics_fixture.hpp::verify_cpp_fixed_broadcast` 用一个 8-byte zero-stride scalar backing，为 `{4}`、`{UINT64_MAX}` 和 `{UINT64_MAX,2}` 三种 shape 发布 full-coverage Float64 Results。`test_plugin_registry.cpp` 和 `test_result_diagnostics.cpp` 都会调用此 helper。每个 output 的 payload cap 为 8 bytes，因此两个独立 Result 可在 16-byte Root Payload 限额内共存；测试读取逻辑域首尾 sample，并检查释放两个输出后 live Payload 回到零。精确的最大 sample count 报告 `computed_elements == UINT64_MAX` 且 `computed_elements_saturated == false`。额外 tensor case 的两个 tensor member 合计有 `UINT64_MAX + 1` 个逻辑样本，但共享一个 8-byte backing；它仍能发布，并将计数饱和为 `UINT64_MAX`。

`publish_tensor_kernel` 为请求区域借出可写 windows。对于 spatial tensor，完整逻辑 sample 的字节几何可表示时，builder 使用 planar rectangles。如果 layout 非 spatial、sample count 不可表示，或完整 sample count 乘 element width 会溢出，builder 会在 Root limits 内为请求区域分配 affine backing。此回退保留 tensor spec、spatial layout、全局 sample 坐标和 certified coverage，只改变物理 writer backing。Callback 必须使用每个 window 的 `spec()`、`region()`、`sample_axis()`，以及 `row_run()` 或 `rectangle_run()` 返回的 strides；不能假设 backing 是 planar 或行连续。巨大逻辑 spatial domain 上的 sparse request 按实际可写区域计费；更大的请求区域仍须满足 Root payload 与 work limits。

```cpp
Status ResultBuilder::publish_tensor_kernel(
    std::uint32_t slot, const Footprint& samples,
    const std::function<
        Status(const ResourceVector<ResultTensorWriteWindow>&)>& write,
    const CancellationToken& cancellation = {});
```

Footprint 重载在一个事务中发布一组精确的、互不相交的 canonical box。Builder 在运行任何 producer 代码之前复制 `samples`，因此 callback 修改调用方的 footprint 对象不会改变发布集合。只有一个 box 的 footprint 走上述 Region 路径。有多个 box 时，footprint 必须已按 tensor 的 observation 分组闭合（`close_samples` 返回同一集合），不能与已有 coverage 重叠；违反时返回 `TypeMismatch`。Callback 按 `boxes()` 顺序为每个 box 收到一个 write window。空洞不获得存储，也不获得 coverage。Empty footprint 跳过 callback 和 payload work。

Callback 在 publication mutex 之外运行；运行期间同一 producer 的 mutation 和 seal 会被拒绝。Callback 返回后，builder 重新检查 owner、revision 和 sticky failure，再检查取消。返回的错误、抛出的异常（分配失败为 `ResourceExhausted`，其他为 `OperationFailed`）、取消或 revision 变化（`Stale`）都会记录 producer failure、释放缓冲且不认证新的 coverage；先前认证的前缀仍可读取。通过这些 windows 写入的 worker 必须在 callback 返回前完成，因为 windows 和原始指针在那时失效。成功时，builder 在一个新 revision 上一起提交这些 box、coverage。该重载每发布一个样本计入一个 Root work 单位，每个 box 计入 Host metadata；字节大小必须满足 builder 的 tensor 存储增长上限。

当一个 slot 的首次发布就是这种多 box 发布时，它的 affine 片段与 canonical coverage box 一一对应。`acquire_tensor` 随后用二分查找定位包含请求起点的片段。落在单个片段内的请求得到一个 view；跨片段的请求先整体对 coverage 授权，再与每个片段各裁剪一次。所得 window 上的 `row_run()` 和 `rectangle_run()` 同样用二分查找定位片段。该 slot 之后的任何发布都会使其 window 回到逐片段查找。两种情况下，callback 和读取方都使用 `row_run()` 与 `rectangle_run()` 报告的 stride。

发布的 Result 记录有序 source ObjectIds，用于 lineage 与 dependency 报告。Association 是 metadata，不是 input payload 的 strong owner，也不授予读取权限。`ResultObjectInputs`、`ResultTensorInput` capabilities 和 retained windows 提供上文说明的活动 owner 与授权路径。Physical tensor view 也会保留它发布的所有 source Results。ICC profile 或 OCIO 配置字节等不可变算子资源不是 Result 成员。静态 preparation 返回它需要的显式 `ResourceIdentity` 列表；compiler 从传给 `Compiler::compile` 的 `ResourceSet` 中精确选择这些条目，缺失时失败，plan 和执行状态保留所选集合。因此 Result 自身不会让颜色配置保持存活。Public `ResourceMap` 和 `ResourceVector` containers 也携带 allocator 与 managed ownership；复制的 results、names 和 observations 会在容器及其所含 `ResultRef` owners 释放前持续占用 root 计量的 metadata。Consumer 应将这些复制与 payload 一起纳入预算。该策略偏保守，因此仍存活的 owners 可能保留比最终输出本身所需更多的 backing。

## 端口校验器与诊断报告

内核为样本合法性和有界报告提供通用扩展点，不内置领域校验器：算子 SDK 自行注册，例如 `plugins/ops/common/domain/contracts.cpp` 中的 RGBA、LUT table 与 LUT report 校验器。

```cpp
struct ExtensionIdentity final {
  std::string key;
  std::uint32_t version = 1;
  std::string implementation;
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

struct OperationPortConstraint final {
  std::vector<ExtensionIdentity> validators;  // 有序；参与 identity
  // ... dtype、rank、schema id/version、tensor key、scalar bounds
};

Status OperationRegistry::register_validator(ValidationDefinition);
Status OperationRegistry::register_diagnostic(DiagnosticDefinition);
```

端口通过 `OperationPortConstraint::validators` 显式绑定校验器。每一项命名已注册的 key、version 和 implementation；registry 拒绝缺失的 identity、version 或 implementation 冲突、重复 key、格式错误的定义以及 `freeze()` 之后的任何注册，且不留下部分修改。绑定的 identity（含 version 与 implementation）参与 operation identity，因而参与 plan cache key。地址、回调指针和报告内容不参与。

校验分两个阶段：

- `prepare` 在 operation preparation 期间、registry 锁之外执行一次。它接收端口 schema、静态参数，以及限定为该 operation preparation 所选 identity 的 `ResourceSet`，返回拥有所有权的不可变状态。
- `validate` 在入口线程上针对每次新授权或发布执行。`ValidationContext` 只暴露宿主授权的读取：`acquire_tensor` 和 `read_field` 对当前 coverage 之外的区域或行范围返回 `InvalidArgument`/`UnauthorizedRead`。工作量通过 `consume` 扣除，`scratch_bytes` 在进入前准入。校验器不得保留 context 或任何借用对象。

Coordinator 在三个位置调用校验器：

| 位置 | `validate` 可见的 coverage | 失败类别 |
| --- | --- | --- |
| 输入 tensor Need | 一个 slot 新授权的 sample footprint；拒绝 field 读取。仅当 schema 只有一个 tensor、没有 field 且授权覆盖整个 slot 时 `complete` 为 true。 | Workflow input：`InvalidArgument`。上游产生的输入：`OperationFailed`。Origin 为 `Domain`。 |
| Whole 输入消费（有 field、没有 tensor，或存在 `requires_complete` 校验器） | 完整的 sealed Result。 | 同上。 |
| 输出发布 | 只包含自上次已校验 revision 以来新增的 coverage 和 field 行；`complete` 等于 descriptor 的 sealed 状态。 | `OperationFailed`，origin 为 `Domain`。 |

带 `requires_complete` 的校验器需要完整的 `CompleteBundle`。在输入端口上，operation 必须请求 Whole 授权；对该端口发出 tile Need 会作为 protocol 错误失败，内核不会自行扩大授权。Tensor 路径会跳过已由 Whole 路径校验过的输入的逐 slot 检查，因此同一次消费不会被校验两次。

Completed-result cache 命中时，输入校验器仍会在重放的 Needs 上运行，但不重复计算。因此非法 workflow input 会在返回缓存输出之前以 `InvalidArgument` 失败；校验器在 prepare 中使用的静态参数发生变化时，plan cache key 也随之改变。

一次 `validate` 调用中的第一个服务失败优先：读取失败而回调忽略它时，该调用仍返回该读取失败。`prepare` 或 `validate` 抛出的异常会转换为带类型的失败。`ValidationDefinition::owner` 保留回调代码；宿主只在回调对象和 prepared 校验器状态销毁之后才销毁它，因此调用方释放 registry handle 后，prepared operation 仍可工作。

`OperationTraits::diagnostics` 列出 operation 可能产生的报告类型。回调运行前，宿主为这些类型初始化 Root 拥有的 `DiagnosticAccumulator`。回调通过 `ResultProgramPhase::report_diagnostic`（C 为 `report_diagnostic`）按声明序号报告固定大小的字节记录；`DiagnosticDefinition::validate` 检查记录，`merge` 写出完整的新记录且不修改任一输入。报告失败时已累计字节保持不变。`OperationTiming::reports` 携带拥有所有权的 accumulator；`find(identity)` 返回合并后的字节，该类型从未报告时返回 `NotFound`。内核不解释这些字节。数值工作报告是其中一种类型，定义在 SDK 头文件 `photospider/ops/numeric/diagnostics.hpp` 中。

Operation 可以为失败附加不透明的领域码。`Status(ErrorCode, message, DomainErrorCode, FailureDetail)` 把它保存到 `FailureDetail::domain_code`；C 回调在运行服务或校验服务上调用 `fail_domain(status_code, domain_code)`。内核原样保留该码，不作解释；第一次记录的领域失败不会被后续回调失败或诊断失败替换。

## 限制与错误处理

每个 `ExecutionContext` 都会创建 managed `ResourceBudget`。若未设置 `ExecutionContextConfig::managed_resources`，context 使用默认构造的 `ResourceLimits`；显式值用于配置容量。`maximum_live_bytes` 也会限制 Payload 维度。`ExecutionContext::resource_budget()` 返回供临时存储客户端共享的 root handle，其 leases 可以比 context 存活更久。

Execution root 按各自配置资源维度计量 payload/backing、work、stages、continuations、queue entries、metadata 和保留的 owners。各类资源都有有限上限。payload 或 owner 准入超限时返回资源错误，不返回不完整 witness。该 managed-capacity 模型计量已 instrument 的资源，不构成 RSS 上限。

Output 可声明 `maximum_output_payload_bytes`。对于 structured Result operation，host 在发布时按每个 output 的 cap 检查 Result 的实际物理 backing，对共享 owner 去重，并排除仍由已暴露 Result 或已授权 tensor grant 保活的 source allocations，其中包括由 `ResultTensorInput` 保留的 Whole 私有 input backing。检查只保存 source 的 weak references，因此 cap 不会延长输入 payload 的生命周期。Root 仍会计费 source 和收集后的 input backing；output cap 只衡量新增的 output payload。每次 prefix publication 都检查完整 Result，因此随着 backing 增加，检查覆盖累计结果。Direct singleton、compiled execution 均采用相同规则。首次 eligible poll 会在进入 callback 前从 Result Root 分配 cap guard 及其 shared control block；Metadata 容量不足会在 callback 执行前拒绝该 poll。Owner 遍历与比较会消耗通常的 Run/Root work。超过 cap 返回带 `CapacityLimit` 的 `ResourceExhausted`。Workspace、metadata 和算子资源准入分别计量。检查发生在 Result publication 时，因此 trusted callback 在发布前已经进行的 Root allocations 仍照常计费，cap 不会阻止这些分配。

Result publication errors 具有 sticky 语义。取消和 stale execution 会在已准入 callbacks 返回后终结活动状态。Append 或 tensor publication 失败后，后续成功 callback 不能将其修复为成功；先前已认证的 prefix 仍可通过 captured descriptor 读取。最后一个 owning Result/read-window 被释放时，backing 和保留的 associations 才退休。

内核构建 注册行为回归测试。`tests/integration` 中的注册项是当前可执行覆盖范围；本规格说明运行时契约，不维护历史测试清单。
