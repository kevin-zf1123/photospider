# 原子错误与数值质量

安装消费方[示例](../../../examples/atom_outcomes_workflow/README.md)运行真实区域 source、编译后的 DAG 回调和拥有型 sink 观察。

## 模块边界与公开类型

```cpp
class QualityReport final {
 public:
  static Result<QualityReport> measured_residual(
      std::string_view snapshot, std::uint64_t dimension, double residual,
      const BufferAllocator& allocator);
  static Result<QualityReport> certify_integer_diagonal(
      std::string_view snapshot, const std::int64_t* diagonal,
      const std::int64_t* estimate, const std::int64_t* rhs,
      std::uint64_t count, const BufferAllocator& allocator,
      const std::function<Status(std::uint64_t)>& consume_work,
      const CancellationToken& cancellation = {});
};

struct AtomKey final {
  std::uint32_t output_index = 0, rank = 0;
  std::array<std::uint64_t, 8> coordinate{};
};

struct AtomObservation final {
  ResourceString name;
  ValueRef output;
  AtomKey key;
  Result<ResultRef> outcome;
  std::optional<QualityReport> quality = {};
};
```

Host 持有已经准入的 observation 和有界错误诊断；operation callback 按契约返回逐观察结果并附质量证据。代码片段只列相关声明；include 与外围类型定义省略。

## 执行与状态机

生产者可返回 Need、局部 observation 结果或请求级失败。Host 在交付任何成员结果前校验完整 callback envelope。Need 只挂起对应成员，由 coordinator 解析精确输入需求。

```text
Pending -> Ready -> Need -> Waiting --supply--> Ready
               |                               |
               +-> Success -> Terminal          +-> Success -> Terminal
               +-> Local failure -> Terminal

invalid envelope -> callback round 返回 Protocol failure
ValidationDomain failure -> 覆盖范围内 Ready/Waiting 成员失败
waiter cancellation -> 该 waiter 的 observation 停止
```

## 联合观察执行

`OperationTraits::joint_contract = 2` 接受 1..64 个不同 `AtomKey`。键由声明顺序中的输出编号和 1..8 维逻辑观察坐标组成。对于 structured Result tensor，坐标使用 tuple-channel 与 atomic trailing axes 合并后的 full-sample 剩余轴；batch axes 仍保留为坐标。未使用的坐标槽为零。Contract 1 按不同输出分组；contract 2 使用完整 `AtomKey` 标识每个观察点。`ResultJointOutcome` 和 Result joint callback phase 使用该键。

每轮仅提供当前 Ready 成员，每个成员必须返回一个 Success、Need 或失败。 宿主在交付任何回复前检查整轮成员集合、payload 元数据、失败范围和质量附件。 未知、重复、遗漏、Waiting 或 Terminal 键都是协议错误。Need 挂起对应成员， 直到其精确授权输入范围被供给。每个成员只终结一次，局部失败不要求兄弟失败。 回调不得保留借用的 phase 或等待上游执行，状态与结果各自拥有已准入存储。

每个 `ResultJointContinuation::poll` 接收就绪成员及其借用的 phase services。Host 在交付成员回复前会校验整组 outcome；callback 返回后，借用的 phase 即失效。Coordinator 会先登记 callback 返回的 Needs，再执行上游读取，并计入共享 workspace 和 Run/Root work，同时保留首个 failure。它不会将不同请求合并为新的语义验证域。Contract-2 group failure 是终局失败；contract 1 仍保留单独的可选准入 fallback。

CPU 协调器收集至多 64 个已有需求坐标，支持同一输出的多个坐标，并在执行上游读取前登记所有 Need。只去重完全相同的 source 传输，不将不同请求合并为新的语义验证域。contract 2 的外层组失败不触发 singleton 重试。contract 1 保留原有可选联合准入失败后的行为。

`ExecutionContext::execute_atoms(plan, bindings, requested, token, options)` 从受 managed Root 管理的 CPU structured Result dependency plan 收集持有资源的 `AtomObservation`。每个请求 output 必须声明 Atomic、PerAtomOutcome 和 joint contract 2。Coordinator 会先验证所有请求 output 和合计 observation 数量，再创建 Actor 或调用 producer；数量受 `maximum_atom_observations` 和 65,536 硬上限约束。空需求返回空 atom 集合，不启动 producer。

Tensor output 的 `requested` Footprint 与 operation Need 都采用完整 sample shape，batch axes 排在 cell axes 前。Tuple-channel 和 atomic trailing axes 合并为一个 observation；剩余 batch/cell 坐标组成 `AtomKey`。每条记录返回 `Result<ResultRef>`，成功时保留实际 Result 与 dependency evidence。局部 Atom、ValidationDomain 或 Group failure 保留在对应 observation 中，并可携带匹配的 quality report。Protocol、Run、Waiter 和调用级 cancellation failure 则终止整个调用。Contract 2 可将最多 64 个单 observation query 分组，包括同一 output 的不同 Q；关闭分组仍使用单成员 joint callback，group failure 不会回退为 singleton。此接口没有 GPU 路径。其他 Value 执行入口仍独立存在。

### Structured Result direct contract 2

`OperationRegistry::start_result_joint` 也支持 direct CPU Result continuation 的 contract 2。注册时 output 必须为 Atomic，并使用 `PerAtomOutcome` delivery。一次 start 接受 1..64 个不同的 `AtomKey`；多个成员可以指向同一 output，但 coordinate 必须不同，且该 output 的成员共用一个 tensor slot。Direct registry 调用方自行驱动 continuation；`ExecutionContext::execute_atoms` 通过 structured coordinator 供给 Needs、校验 publication 并收集命名 observations。

`result_atom_key(query)` 标识选定 output 的一个逻辑 observation。对于 tensor query，host 将 tuple-channel 与 atomic trailing axes 闭合为一个 observation，并从 key 中省略这些轴。其余 sample axes 按原顺序保留，包括 batch axes；每轴必须选定一个 coordinate。因此 channel samples 不是独立 atoms。`result_observation_domain(query)` 返回这些剩余轴的完整固定 domain，不随请求子集变化。只有 collection 的 output 使用 coordinate 为零的 singleton key 及 singleton domain。

每个 Ready member 返回一个以 `AtomKey` 标识的 `ResultJointOutcome`：Result Need、完整 publication 或 typed failure。Need 不携带 quality evidence。Atom failure 必须指定精确 key。`ValidationDomain` failure 必须使用 Domain 或 Schema origin，且与 output 完整固定 observation domain 一致；它会作用于调用中的匹配 Ready 和 Waiting 成员。

Contract 2 可为成功 publication 或匹配的 Domain failure 附加 `QualityReport`。成功估计必须是通用 Float64 向量，每个 report dimension 对应一个元素；host 读取成员估计，并将认证行与实际值核对。Measured evidence 要求估计有限，但不声称有认证误差界。Measured report 可以附在 Domain failure 上。Report 的 snapshot label 与 joint query 的路由 `snapshot_identity` 相互独立。

Report 必须属于成员的 ResourceBudget，并由 `phase.resources.allocator()` 以及其 phase allocator 或 shared workspace allocator 持有。Continuation 还会验证 phase/member allocator 与 Root 的非空 accounting domain 相同；`BufferAllocator::same_owner` 只比较该 domain，不比较 quota 或 provenance，也不会授予 payload 读取权限。检查 estimate 时发生的读取错误保留原状态。取消会清除 report 并返回成员局部 Cancelled outcome；estimate 非有限或认证行不匹配会设置 `FailureReason::InvalidQuality`。

Direct C Result joint table 也支持 contract 2。它会复制带有有界诊断的 Atom 或 ValidationDomain typed failure record；producer identity 由 host 填写，并执行上文相同的精确 Atom 和固定 domain 规则。共享 poll service 可创建 Measured 与 IntegerDiagonal report，并返回仅在该 callback epoch 有效的 handle；host 会在 callback lease 退休前复制附加的 owning report。Report 可附加到成功 publication，或携带 Measured evidence 的 Domain-origin Atom/ValidationDomain failure。Poll 即使最终返回 Need 也可创建 report，但 Need outcome 不能附加它；未使用的 report 随 lease 退休。Contract 1 不携带 quality evidence。C singleton operation table 仍没有 typed Atom 或 quality attachment。

Structured execution 已会用 contract 1 或 2 调度符合条件的 CPU Result query。Contract 2 可将最多 64 个单 observation query 分组，也可分组同一 output 的不同 query。每个 C2 member 对应一个 output observation，但它的输入 Tensor Need 可以跨多个 observation。如果输入 Need 指向计算型 C2 producer，coordinator 会将 samples 展开为独立的 singleton-atom 请求，最多 `min(65,536, maximum_boxes)` 个，并按每组最多 64 个请求调度，再供给由原始 Results 支持的分片 `ResultTensorInput`。每个 piece 保留原 Result owner、captured descriptor 和获准 samples；coordinator 不创建聚合 Result 或新的 validation domain。其他路径仍支持更宽的 computed Tensor Need。关闭 joint grouping 时，contract 2 仍使用必需的单成员 joint callback；contract-2 group failure 不会回退为多个 singleton callback。Run ledger 按编译后的 producer semantic identity、output 和 tensor slot 保存 ValidationDomain failure 与 semantic-terminal observation。同一 compiled producer 的 semantic alias 共享这项终局记录；failure detail 仍指出实际失败的 workflow node。同一 cohort 内一个 output 只使用一个 tensor slot；其他 slot 的 query 由后续独立 cohort 处理。Ledger 跨 cohort、completed-cache 复用和 Actor 退休继续有效。Coordinator 在调用 callback 前为每个 query 分配 payload-free failure 与 quality record，记录终局结果时无需再分配。

复合 capability 只授权各 piece 获准 samples 的并集。单个 Result 支持时，`object_id()` 返回 source identity；多个原始 Result 支持时返回零。零表示没有单一 Result 身份，不授权读取 payload 或调用 `result_descriptor`。Phase association 仍记录每个真实 input port 和 source ObjectId。获取的 windows 可在不复制 payload samples 的前提下组合获准 backing pieces，同时保留所有原始 Result owners，包括由 fields、associations 和 resources 持有的 owner。发布 view 仍要求物理存储构成一个合法 affine 表示，因此不保证任意分片都能发布为一个 view。此展开只处理 tensor Need，不聚合 object 或 field support。Descriptor-only Empty C2 输入和 GPU C2 joint 不属于此路径。

Shared producer entry 会将 quality 与 publication 或 failure 一起保留，供 waiter 读取。Domain failure 的身份和 evidence 会继续向下游传播；成功 transformation 不继承来源 report。带 quality 的 result 不进入 completed-output content retention。Structured Result joint execution 尚无 GPU 路径。

## 失败身份与范围

`Status.code` 是粗粒度分类，`reason` 是机器可读原因，`detail` 描述来源与范围。 诊断字符串不用于分支判断，Unspecified 表示旧接口仅提供错误码。

- Atom 指向生产者的一个逻辑观察。
- ValidationDomain 指向固定语义域；contract 2 当前接受完整不可变输出观察形状。 域失败传播到覆盖的 Ready/Waiting 成员，禁止撤销此前已终结的语义观察， 包括同一 Run 的更早物理批次。Run 保存失败全域以约束后续坐标。 本轮请求子集不能定义验证域。
- Association 指向发布前验证的 Result ObjectId；观察者可见前检查关联字段。
- Group、Run、Waiter 描述操作影响范围，不在任意坐标编造语义失败值。

宿主将 node_id 或 input_id 绑定到真实失败来源，传播时保留该身份及原始 atom。 AtomObservation 的 output/key 指向请求消费方，因此可与上游原因的坐标不同。 source 传输失败保持 Io/Group 和原输入身份。失败没有成功 Value 或成功依赖证书。

读取、工作量和分配服务失败具有粘滞性，回调捕获异常或忽略错误后也不能成功发布。 已检测 Protocol 错误优先于后续取消。共享结果的等待者取消仍局限于该等待者； 已保存的 Protocol 终结可继续观察。错误成员集合或 payload 在本轮成功交付前拒绝。

ResultRef、结构化 Actor 和共享等待者以至多 256 字节内联诊断保存首个原因与范围。 宿主仅为同一原因补充缺失的生产者身份/范围，不覆盖已指定的上游身份。诊断复制分配失败时保留固定机器字段并返回空字符串。内联空间计入所属 managed 对象。

## 算法与数值质量界

QualityReport 由命名工厂创建并持有不可变分配，调用方不能用枚举标志升级证据。 Measured 有限残差没有认证误差界。CertifiedBound 当前只用于经检查的整数对角系统：1..4096 行、非零对角元、各输入绝对值不超过 2^26；快照名称是 1..256 个可打印 ASCII 字节。报告保留全部精确整数证明行。

对保存的对角系统，令 $R$ 为最大精确残差，$m$ 为对角元绝对值的最小值。

$$
R = \max_i |a_i x_i - b_i|, \qquad m = \min_i |a_i|, \qquad \|x-x^*\|_\infty \le \frac{R}{m}.
$$

工厂检查 1..4096 行、非零对角元和不超过 $2^{26}$ 的输入绝对值。乘积与残差是小于 $2^{53}$ 的精确整数，因此残差舍入误差为零。实现按 binary64 最近舍入计算正商，再向正无穷移动一次；`R=0` 时误差界精确为零。

```cpp
volatile double quotient = static_cast<double>(residual) / static_cast<double>(minimum);
const double bound = residual
    ? std::nextafter(quotient, std::numeric_limits<double>::infinity())
    : 0.0;
```

该证明不适用于任意 Float64 系统、仅基于残差的停止规则、其他范数或相对误差。

运行时附件当前要求无 facet 的 Float64 向量，维数匹配报告。成功原子的实际估计必须精确匹配认证证明行，因此整数 2^24+1 的报告不能认证经 Float32 舍入为 2^24 的输出。Need 不携带质量。Domain 失败可携带 Measured，随原失败传播到下游； 它不描述消费方的新估计。成功变换不隐式继承其他算子的报告。可选结果缓存跳过携带质量的值，拥有资源的执行中结果与返回值保留报告。

数值证据与依赖保证相互独立，Conservative 保持 Conservative。资源计账遵守 [已有范围](Managed-Resources.zh.md)；数值误差界和 Python 模型均不构成产品 RSS 硬上界。

## 限制与调用方处理

Atom collection 仅适用于受管 CPU structured Result dependency plan，并受 observation 数、work 和 root capacity 限制。调用方应把每个失败 outcome 视为带范围的观察错误；普通 `execute` 仍按 fail-fast 处理。其他 Value 执行入口仍独立可用。质量证据描述数值估计，与依赖完整性相互独立。
