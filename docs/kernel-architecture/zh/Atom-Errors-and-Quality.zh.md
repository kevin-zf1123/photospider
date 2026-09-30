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
  Result<ValueFragments> outcome;
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

`OperationTraits::joint_contract = 2` 接受 1..64 个不同 `AtomKey`。键由声明顺序中的输出编号、1..8 维逻辑观察坐标组成；AtomKey 在受支持的图像观察模型中使用 HW 坐标，未使用的坐标槽为零。 contract 1 按不同输出分组；contract 2 使用完整 `AtomKey` 标识每个观察点。C++ outcome、progress、supply 和 pending-read 接口使用完整键。

每轮仅提供当前 Ready 成员，每个成员必须返回一个 Success、Need 或失败。 宿主在交付任何回复前检查整轮成员集合、payload 元数据、失败范围和质量附件。 未知、重复、遗漏、Waiting 或 Terminal 键都是协议错误。Need 挂起对应成员， 直到其精确授权输入范围被供给。每个成员只终结一次，局部失败不要求兄弟失败。 回调不得保留借用的 phase 或等待上游执行，状态与结果各自拥有已准入存储。

联合 phase 及借用函数对象保存在一个宿主准入的缓冲区中。准备阶段每成员只计一次阶段，真实回调运行在浮点环境内，验证后逐个结束成员，不递归嵌套 Session::poll。 直接宿主可用 member_phase_bytes() 预算共享/成员 scratch 之外的适配空间。 没有 checkpoint 缓存时仍提供经检查的 miss 和纯 block 执行。

CPU 协调器收集至多 64 个已有需求坐标，支持同一输出的多个坐标，并在执行上游读取前登记所有 Need。只去重完全相同的 source 传输，不将不同请求合并为新的语义验证域。contract 2 的外层组失败不触发 singleton 重试。contract 1 保留原有可选联合准入失败后的行为。

`ExecutionContext::execute_atoms` 为启用 managed resources 的 CPU Value 依赖计划收集持有资源的 `AtomObservation`。命名输出必须是 Atomic/PerAtomOutcome。 Planar image plan 返回 `TypeMismatch`，所以 HW 坐标仅定义 AtomKey 表示，不表示当前支持 image atom execution。 `maximum_atom_observations` 默认值与硬上限均为 65536，同时受 root 容量和工作量限制。空需求返回空观察集合。普通 `execute` 保持遇错即返回。结构化 Result 使用既有三种发布策略；此接口不增加持久执行状态或 GPU 批次后端。

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

Atom collection 仅适用于受管 CPU Value dependency plan，并受 observation 数、work 和 root capacity 限制。调用方应把每个失败 outcome 视为带范围的观察错误；普通 `execute` 仍按 fail-fast 处理。质量证据描述数值估计，与依赖完整性相互独立。
