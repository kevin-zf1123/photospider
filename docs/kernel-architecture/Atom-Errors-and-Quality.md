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
  std::uint64_t node_id = 0, input_id = 0;
  std::optional<std::uint64_t> call_sequence = {};
  std::optional<std::uint32_t> task_id = {};
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
- Group, Run and Waiter describe operational extent and do not invent a failed semantic value at an arbitrary coordinate.

`FailureScope` uses `Unspecified=0`, `Atom=1`, `Group=4`, `Run=5`, and `Waiter=6`. `AtomKey` identifies an output index and 1..8 coordinates, with unused coordinates zero. `FailureDetail` carries `origin`, `scope`, optional `atom`, `node_id`, `input_id`, optional `call_sequence`/`task_id`, and opaque `domain_code`. The operations SDK's `InvalidAssociation` domain code is available to explicit domain operations. Ordinary LUT report producers, readers and Gates trust table/configuration associations without testing them. These numeric scopes classify failure extent; they do not define per-sample dependency semantics.

Fixed invocation ABI 8 returns `code`, `reason`, opaque `domain_code` and a 256-byte message array including its terminator through `ps_kernel_status_v8`. The kernel does not interpret domain codes. C++ callers may construct `Status` with `DomainErrorCode`; C kernels fill the status record directly. Stable operation domain codes are defined in `photospider/ops/errors.hpp`.

The coordinator assigns Backend/Run origin and scope to a failed task and supplies its actual `node_id` and `task_id`. Observer exceptions use Observer/Run with the source call and dynamic `call_sequence`. These identity fields and the optional atom describe diagnostic provenance; they do not establish successful output or dependency readiness.

During a Run, Cancelled outranks Stale, which outranks other abort causes; the first observed cause within a category wins. This ordering applies until the Run locks its terminal outcome and includes completion-protocol faults. Accepted work must finish cleanup before a terminal result is returned. Ordinary task failures retire in CallGroup order; a younger failure does not skip an unsettled older call.

Task failure messages are copied into storage reserved before issue. Invalid or duplicate completions use an empty-message `Internal`/`MalformedEnvelope` status to keep that failure path allocation-free. Failed Result construction state separately retains the first failure's fixed fields and up to 256 diagnostic bytes. If copying an owning `Status` cannot allocate, the fixed fields survive with an empty message. Repeated `RunHandle::join()` similarly preserves the locked error category and identity.

## Algorithms and mathematical quality bounds (operations SDK)

`QualityReport` is an operations-SDK type in `photospider/ops/numeric/quality.hpp`; the kernel neither creates nor reads it. It is an immutable owning allocation created only by named factories. Its evidence enum cannot be promoted by a caller-supplied flag. Measured finite residuals carry no certified error bound. CertifiedBound is currently available only from the exact integer diagonal bound calculation. Its 1..4096 rows, nonzero diagonal, magnitudes at most 2^26, array/callback validity and 1..256 printable ASCII snapshot bytes are unchecked caller premises. The factory constructs the residual and bound and retains all exact proof rows without an input or publication-validation pass. `proof_row` trusts its index; it does not check runtime structure or numerical legality.

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

Numerical evidence and dependency guarantees are independent. Managed resource accounting retains its [documented scope](Managed-Resources.md); a numerical bound does not establish a product RSS hard bound.

## Limitations and caller handling

Quality evidence describes numerical estimates independently of task completion and dependency readiness. `DiagnosticAccumulator` is an explicit report-merging API (see [Structural contracts and diagnostic records](Global-Results.md#structural-contracts-and-diagnostic-records)); fixed invocation ABI 8 has no report-submission callback, and `QualityReport` remains an independent SDK object.
