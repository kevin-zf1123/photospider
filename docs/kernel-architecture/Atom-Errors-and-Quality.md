# Atom outcomes, failure scope and quality evidence

## Scope and public types

The kernel owns failure categories, scopes, and an opaque domain-code channel. Domain-specific failure meanings and numerical quality reports belong to the operations SDK.

```cpp
// Kernel: photospider/core/status.hpp, photospider/core/atom_key.hpp
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
  std::uint64_t domain_code = 0;  // opaque, preserved without interpretation
};
struct AtomKey final {
  std::uint32_t output_index = 0, rank = 0;
  std::array<std::uint64_t, 8> coordinate{};
};

// Operations SDK: photospider/ops/errors.hpp
namespace ps::ops_error {
inline constexpr DomainErrorCode InvalidAssociation{0x50534f5000000001ULL};
inline constexpr DomainErrorCode AssociationUnderflow{0x50534f5000000002ULL};
inline constexpr DomainErrorCode DivideByZero{0x50534f5000000003ULL};
inline constexpr DomainErrorCode InvalidQuality{0x50534f5000000004ULL};
}

// Operations SDK: photospider/ops/numeric/quality.hpp
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

## Failure identity and scope

`Status.code` retains the coarse error category. `reason` is a machine-readable cause, and `detail` records origin and scope. Diagnostic strings are not branch conditions. Unspecified fields are the legacy category-only projection.

- Atom identifies exactly one logical producer observation.
- ValidationDomain identifies a fixed logical range.
- Association identifies the immutable Result ObjectId being validated before publication. Associated fields are checked before observers see that result.
- Group, Run and Waiter describe operational extent and do not invent a failed semantic value at an arbitrary coordinate.

The host binds `node_id` or `input_id` to the actual source of a failure. Propagation retains that identity and the original atom. A transport read failure remains Io/Group with its input declaration identity. Failure records have no success Value or success dependency certificate.

`FailureReason` contains only kernel-generic causes. An operation that needs a finer domain meaning constructs `Status(code, message, DomainErrorCode{value}, detail)`, or calls `fail_domain(status_code, domain_code)` from C. The kernel copies the value into `FailureDetail::domain_code` and transports it unchanged; it does not interpret or validate the number. The operations SDK defines its stable codes in `photospider/ops/errors.hpp`, for example `ops_error::DivideByZero` and `ops_error::InvalidAssociation`. A caller that branches on these codes includes that SDK header.

Read, work and allocation service errors are sticky, including exceptions caught by a callback. A callback cannot replace a prior service failure by returning a value or another same-code status. A recorded domain failure is likewise not replaced by a later callback status, exception, or diagnostic-report failure. A detected Protocol violation survives later cancellation.

Failed ResultRefs and structured actors retain the first cause, reason, scope, and domain code in inline bounded diagnostic storage (up to 256 diagnostic bytes). The host may fill missing producer identity/scope for that same cause; it does not change an already specified upstream identity. Diagnostic-copy allocation failure returns the fixed machine-readable fields with an empty diagnostic. These inline bytes are charged with their owning managed runtime objects.

## Algorithms and mathematical quality bounds (operations SDK)

`QualityReport` is an operations-SDK type in `photospider/ops/numeric/quality.hpp`; the kernel neither creates nor reads it. It is an immutable owning allocation created only by named factories. Its evidence enum cannot be promoted by a caller-supplied flag. Measured finite residuals carry no certified error bound. CertifiedBound is currently available for the checked integer diagonal system factory only, with 1..4096 rows, nonzero diagonal and magnitudes at most 2^26. Snapshot names are 1..256 printable ASCII bytes. The report retains all exact integer proof rows. Malformed report inputs return `InvalidArgument` with domain code `ops_error::InvalidQuality`.

For the stored diagonal system, let $R$ be the maximum exact residual and $m$ the minimum absolute diagonal value.

$$
R = \max_i |a_i x_i - b_i|, \qquad m = \min_i |a_i|, \qquad \|x-x^*\|_\infty \le \frac{R}{m}.
$$

The factory checks 1..4096 rows, nonzero diagonal entries and input magnitudes no greater than $2^{26}$. Products and residuals are exact integers below $2^{53}$, so residual rounding error is zero. The implementation computes the positive quotient in binary64 under nearest rounding and advances it once toward positive infinity; if $R=0$, the bound is exactly zero.

```cpp
volatile double quotient = static_cast<double>(residual) / static_cast<double>(minimum);
const double bound = residual
    ? std::nextafter(quotient, std::numeric_limits<double>::infinity())
    : 0.0;
```

Each proof row is charged through `consume_work` before access; cancellation or work failure returns no report. The proof does not cover arbitrary Float64 systems, residual-only stopping rules, other norms or relative error.

Numerical evidence and dependency guarantees are independent. Conservative relations remain Conservative. Managed resource accounting retains its [documented scope](Managed-Resources.md); a numerical bound does not establish a product RSS hard bound.

## Limitations and caller handling

Ordinary `execute` remains fail-fast. Quality evidence describes numerical estimates and remains separate from dependency completeness. The kernel's generic report channel is `DiagnosticAccumulator` (see [Port validators and diagnostic reports](Global-Results.md#port-validators-and-diagnostic-reports)); `QualityReport` is not transported through it.
