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
  std::optional<AtomDomain> domain = {};
  std::uint64_t association = 0, node_id = 0, input_id = 0;
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
- ValidationDomain 标识一个固定的逻辑范围。
- Association 标识发布前正在校验的不可变 Result ObjectId。关联字段在观察者看到该结果之前完成检查。
- Group、Run 和 Waiter 描述操作范围，不会在任意坐标上虚构一个失败的语义值。

宿主把 `node_id` 或 `input_id` 绑定到失败的实际来源。传播过程保留该身份和原始 atom。传输读取失败保持为 Io/Group，并带有其输入声明身份。失败记录没有成功 Tensor，也没有成功依赖证书。

`FailureReason` 只包含内核通用原因。需要更细领域含义的算子构造 `Status(code, message, DomainErrorCode{value}, detail)`，或在 C 中调用 `fail_domain(status_code, domain_code)`。内核把该值复制到 `FailureDetail::domain_code` 并原样传递；它不解释也不校验这个数字。算子 SDK 在 `photospider/ops/errors.hpp` 中定义其稳定码，例如 `ops_error::DivideByZero` 和 `ops_error::InvalidAssociation`。按这些码分支的调用方需要包含该 SDK 头文件。

读取、工作量和分配服务错误是 sticky 的，包括回调捕获的异常。回调不能通过返回值或另一个同码状态替换先前的服务失败。已记录的领域失败同样不会被之后的回调状态、异常或诊断报告失败替换。检测到的 Protocol 违规在之后的取消中仍然保留。

失败的 ResultRef 和结构化 actor 在有界内联诊断存储（最多 256 个诊断字节）中保留第一个原因、reason、scope 和领域码。宿主可以为同一原因补齐缺失的 producer 身份/范围；它不会改变已指定的上游身份。诊断复制分配失败时，返回固定的机器可读字段和空诊断。这些内联字节与其所属的 managed 运行时对象一起计费。

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

数值证据与依赖保证相互独立。Conservative 关系仍为 Conservative。Managed 资源计量保持其[文档说明的范围](Managed-Resources.zh.md)；数值界不能确立产品 RSS 硬上界。

## 限制与调用方处理

普通 `execute` 仍然 fail-fast。质量证据描述数值估计，与依赖完整性分开。内核的通用报告通道是 `DiagnosticAccumulator`（见[端口校验器与诊断报告](Global-Results.zh.md#端口校验器与诊断报告)）；`QualityReport` 不通过它传递。
