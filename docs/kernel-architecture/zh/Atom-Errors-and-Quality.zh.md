# 原子错误与数值质量

英文权威文档：[Atom-Errors-and-Quality.md](../Atom-Errors-and-Quality.md)。

## 模块边界与公开类型

内核拥有失败类别、范围和一个不透明的领域码通道。领域专用的失败含义和数值质量报告属于算子 SDK。

```cpp
// 内核：photospider/core/status.hpp、photospider/core/atom_key.hpp
enum class FailureReason {
  None = 0, ArithmeticOverflow, InvalidDomain, MalformedEnvelope,
  UnauthorizedRead, StaleHandle, HostException, ShortIo, WorkLimit,
  CapacityLimit, StageLimit, Cancelled, StaleVersion, ViewUnavailable
};
struct DomainErrorCode final { std::uint64_t value = 0; };
struct FailureDetail final {
  FailureOrigin origin = FailureOrigin::Unspecified;
  FailureScope scope = FailureScope::Unspecified;
  std::optional<AtomKey> atom = {};
  std::uint64_t node_id = 0, input_id = 0;
  std::optional<std::uint64_t> call_sequence = {};
  std::optional<std::uint32_t> task_id = {};
  std::uint64_t domain_code = 0;  // 不透明，原样保留且不解释
};
struct AtomKey final {
  std::uint32_t output_index = 0, rank = 0;
  std::array<std::uint64_t, 8> coordinate{};
};

// 算子 SDK：photospider/ops/errors.hpp
namespace ps::ops_error {
inline constexpr DomainErrorCode InvalidAssociation{0x50534f5000000001ULL};
inline constexpr DomainErrorCode AssociationUnderflow{0x50534f5000000002ULL};
inline constexpr DomainErrorCode DivideByZero{0x50534f5000000003ULL};
inline constexpr DomainErrorCode InvalidQuality{0x50534f5000000004ULL};
}

// 算子 SDK：photospider/ops/numeric/quality.hpp
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
```

## 失败身份与范围

`Status.code` 保留粗粒度错误类别。`reason` 是机器可读的原因，`detail` 记录 origin 与 scope。诊断字符串不是分支条件。未指定的字段是仅含类别的旧投影。

- Atom 精确标识一个逻辑 producer 观测。
- Group、Run 和 Waiter 描述操作范围，不会在任意坐标上虚构一个失败的语义值。

`FailureScope` 的数值为 `Unspecified=0`、`Atom=1`、`Group=4`、`Run=5`、`Waiter=6`。`AtomKey` 标识 output index 和 1..8 维坐标，未使用坐标为零。`FailureDetail` 保存 `origin`、`scope`、可选 `atom`、`node_id`、`input_id`、可选 `call_sequence`/`task_id` 和不透明的 `domain_code`。算子 SDK 的领域码 `InvalidAssociation` 表示显式操作数据身份不匹配，例如 LUT 报告测量表的 ObjectId。这些数值范围用于失败分类，不定义逐样本依赖语义。

固定调用 ABI 8 使用 `ps_kernel_status_v8` 返回 `code`、`reason`、不透明的 `domain_code` 及包含终止符的 256 字节消息数组。内核不解释领域码。C++ 调用方可以构造带 `DomainErrorCode` 的 `Status`；C kernel 直接填写 status 记录。稳定的算子领域码定义在 `photospider/ops/errors.hpp`。

Coordinator 将失败 task 的 origin/scope 设为 Backend/Run，并填写实际 `node_id` 与 `task_id`。Observer 异常使用 Observer/Run，包含来源 call 与动态 `call_sequence`。这些身份字段与可选 atom 描述诊断来源，不建立成功输出或依赖就绪关系。

运行期间，Cancelled 优先于 Stale，Stale 优先于其他中止原因；同类原因保留首先观察到的一个。这个顺序在 Run 锁定终态前适用，也涵盖 completion protocol 故障。已接受的工作必须完成清理后才能返回终态。普通 task 失败按 CallGroup 顺序退役；更年轻 task 的失败不会跳过尚未结算的较老 call。

Task 失败消息在 issue 前预留的存储中复制。非法或重复 completion 使用空消息的 `Internal`/`MalformedEnvelope`，保持无分配错误路径。失败 Result 的构造状态独立保留第一项失败的固定字段与至多 256 个诊断字节。复制拥有型 `Status` 时若分配失败，返回固定字段及空消息。`RunHandle::join()` 的重复读取同样保留已锁定的错误类别与身份。

## 算法与数学质量界（算子 SDK）

`QualityReport` 是 `photospider/ops/numeric/quality.hpp` 中的算子 SDK 类型；内核既不创建也不读取它。它是只能由具名 factory 创建的不可变拥有型分配。其证据枚举不能被调用方提供的标志提升。有限的 Measured 残差不带认证误差界。CertifiedBound 目前只由经检查的整数对角系统 factory 提供：1..4096 行、对角非零、幅值至多 2^26。Snapshot 名称是 1..256 个可打印 ASCII 字节。报告保留全部精确整数证明行。格式错误的报告输入返回带领域码 `ops_error::InvalidQuality` 的 `InvalidArgument`。

对所存储的对角系统，令 $R$ 为最大精确残差，$m$ 为对角元素绝对值的最小值。

$$
R = \max_i |a_i x_i - b_i|, \qquad m = \min_i |a_i|, \qquad \|x-x^*\|_\infty \le \frac{R}{m}.
$$

Factory 检查 1..4096 行、对角元素非零、输入幅值不大于 $2^{26}$。乘积与残差是小于 $2^{53}$ 的精确整数，因此残差舍入误差为零。实现以就近舍入在 binary64 中计算正商，再向正无穷推进一次；若 $R=0$，界恰为零。

```cpp
volatile double quotient = static_cast<double>(residual) / static_cast<double>(minimum);
const double bound = residual
    ? std::nextafter(quotient, std::numeric_limits<double>::infinity())
    : 0.0;
```

每个证明行在访问前通过 `consume_work` 计费；取消或工作量失败时不返回报告。该证明不覆盖任意 Float64 系统、仅基于残差的停止规则、其他范数或相对误差。

数值证据与依赖保证相互独立。Managed 资源计量保持其[文档说明的范围](Managed-Resources.zh.md)；数值界不能确立产品 RSS 硬上界。

## 限制与调用方处理

质量证据描述数值估计，与 task 完成和依赖就绪相互独立。`DiagnosticAccumulator` 是显式报告合并 API（见[结构契约与诊断记录](Global-Results.zh.md#结构契约与诊断记录)）；固定调用 ABI 8 没有报告提交回调，`QualityReport` 保持为独立 SDK 对象。
