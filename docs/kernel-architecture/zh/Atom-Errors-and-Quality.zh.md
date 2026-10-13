# 原子错误与数值质量

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

```

## 失败身份与范围

`Status.code` 是粗粒度分类，`reason` 是机器可读原因，`detail` 描述来源与范围。 诊断字符串不用于分支判断，Unspecified 表示旧接口仅提供错误码。

- Atom 指向生产者的一个逻辑观察。
- ValidationDomain 指向固定语义域。
- Association 指向发布前验证的 Result ObjectId；观察者可见前检查关联字段。
- Group、Run、Waiter 描述操作影响范围，不在任意坐标编造语义失败值。

宿主将 node_id 或 input_id 绑定到真实失败来源，传播时保留该身份及原始 atom。 source 传输失败保持 Io/Group 和原输入身份。失败没有成功 Value 或成功依赖证书。

读取、工作量和分配服务失败具有粘滞性，回调捕获异常或忽略错误后也不能成功发布。 已检测 Protocol 错误优先于后续取消。

ResultRef、结构化 Actor以至多 256 字节内联诊断保存首个原因与范围。 宿主仅为同一原因补充缺失的生产者身份/范围，不覆盖已指定的上游身份。诊断复制分配失败时保留固定机器字段并返回空字符串。内联空间计入所属 managed 对象。

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

数值证据与依赖保证相互独立，Conservative 保持 Conservative。资源计账遵守 [已有范围](Managed-Resources.zh.md)；数值误差界和 Python 模型均不构成产品 RSS 硬上界。

## 限制与调用方处理

普通 `execute` 仍按 fail-fast 处理。其他 Value 执行入口仍独立可用。质量证据描述数值估计，与依赖完整性相互独立。
