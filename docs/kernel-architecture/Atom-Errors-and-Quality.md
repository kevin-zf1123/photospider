# Atom outcomes, failure scope and quality evidence

The installed [example](../../examples/atom_outcomes_workflow/README.md) runs regional sources, compiled DAG callbacks and owning sink observations.

## Scope and public types

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

The host owns admitted observations and bounded failure diagnostics. Operation callbacks supply per-observation outcomes and attach quality evidence under the contracts below.

## Execution and state machine

A producer can return a need, a local observation result, or a request-level failure. The host validates the complete callback envelope before releasing any member result. Need suspends only that member while the coordinator resolves its exact input demand.

```text
Pending -> Ready -> Need -> Waiting --supply--> Ready
               |                               |
               +-> Success -> Terminal          +-> Success -> Terminal
               +-> Local failure -> Terminal

invalid envelope -> Protocol failure for the callback round
ValidationDomain failure -> all covered Ready/Waiting members fail
waiter cancellation -> that waiter's observation stops
```

## Joint observation execution

`OperationTraits::joint_contract = 2` accepts 1..64 distinct `AtomKey` members. A key is the declaration-order output index plus a rank-1..8 logical observation coordinate. The key type can represent HW observation coordinates, not channel samples. Unused coordinate slots are zero. Contract 1 groups distinct outputs; contract 2 identifies every member with a complete `AtomKey`. The C++ outcome, progress, supply and pending-read APIs use that key.

Every poll supplies exactly the current Ready members. Each must return one Success, Need, or failure. The host checks the entire envelope's membership, payload metadata, failure scope and quality attachment before delivering any reply. Unknown, duplicate, missing, Waiting or Terminal keys are protocol errors. Need suspends one member until its exact authorized input coverage is supplied. Each member terminates once; a local failure does not require sibling failure. Callbacks cannot retain borrowed phases or block on upstream work. State and returned values own their separately admitted storage.

Joint phases and their borrowed function objects occupy one host-admitted buffer. Preparation charges each member's stage once, the callback runs once under the floating-point environment, and validated replies retire members sequentially. There is no recursive Session::poll stack per member. Direct hosts can budget `member_phase_bytes()` in addition to shared/member scratch. Checkpoint cache absence still supplies checked misses and pure block execution.

The CPU coordinator gathers up to 64 already demanded coordinates, including coordinates of one output, and registers all returned input demands before performing upstream reads. It deduplicates identical source transports; it does not union distinct requests into a new semantic validation domain. Contract 2 never retries an enclosing group failure as singletons. The existing contract 1 optional admission fallback remains its separate behavior.

`ExecutionContext::execute_atoms(plan, bindings, requested, token, options)` collects owning `AtomObservation` records for managed CPU Value dependency plans. Named requested outputs must be Atomic/PerAtomOutcome. The API rejects planar image plans with `TypeMismatch`, so the HW coordinate convention describes the key vocabulary and does not promise image atom execution. A request is bounded by `maximum_atom_observations` (default and hard cap 65536), root capacity and work limits. Empty demand returns no observations. Ordinary `execute` remains fail-fast. Structured Result publication uses its existing CompleteBundle, StablePrefix or IndependentChunks policy. Atom collection does not introduce persistent execution state or a GPU batch backend.

## Failure identity and scope

`Status.code` retains the coarse error category. `reason` is a machine-readable cause, and `detail` records origin and scope. Diagnostic strings are not branch conditions. Unspecified fields are the legacy category-only projection.

- Atom identifies exactly one logical producer observation.
- ValidationDomain identifies a fixed semantic range. Contract 2 currently accepts the entire immutable output observation shape. It propagates a domain failure to covered Ready and Waiting members and rejects late revocation of prior covered semantic terminal outcomes. A poll's requested subset cannot define this domain.
- Association identifies the immutable Result ObjectId being validated before publication. Associated fields are checked before observers see that result.
- Group, Run and Waiter describe operational extent and do not invent a failed semantic value at an arbitrary coordinate.

The host binds `node_id` or `input_id` to the actual source of a failure. Propagation retains that identity and the original atom. `AtomObservation`'s output/key describe the requested consumer, so a downstream coordinate can observe a failure caused by a different upstream coordinate. A transport read failure remains Io/Group with its input declaration identity. Failure records have no success Value or success dependency certificate.

Read, work and allocation service errors are sticky, including exceptions caught by a callback. A callback cannot replace a prior service failure by returning a value or another same-code status. A detected Protocol violation survives later cancellation. Shared-result waiter cancellation remains local; a previously recorded Protocol terminal remains observable. Invalid callback membership or payload rejects the current envelope before its successes escape.

Failed ResultRefs, structured actors and shared waiters retain the first cause, reason and scope in inline bounded diagnostic storage (up to 256 diagnostic bytes). The host may fill missing producer identity/scope for that same cause; it does not change an already specified upstream identity. Diagnostic-copy allocation failure returns the fixed machine-readable fields with an empty diagnostic. These inline bytes are charged with their owning managed runtime objects.

## Algorithms and mathematical quality bounds

`QualityReport` is an immutable owning allocation created only by named factories. Its evidence enum cannot be promoted by a caller-supplied flag. Measured finite residuals carry no certified error bound. CertifiedBound is currently available for the checked integer diagonal system factory only, with 1..4096 rows, nonzero diagonal and magnitudes at most 2^26. Snapshot names are 1..256 printable ASCII bytes. The report retains all exact integer proof rows.

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

The proof does not cover arbitrary Float64 systems, residual-only stopping rules, other norms or relative error.

Contract 2 runtime attachments currently require a facet-free Float64 vector whose dimension matches the report. A successful atom's actual estimate must match the retained certified proof row exactly. A report for integer 2^24+1 therefore cannot certify an output rounded through Float32 to 2^24. A Need cannot carry quality. Domain failures may carry Measured evidence, which follows the original failure through downstream consumers; it does not describe a new consumer estimate. Successful operation transformations do not implicitly transfer another operation's report. Optional result cache retention skips quality-bearing values; owning in-flight/result handles retain their reports.

Numerical evidence and dependency guarantees are independent. Conservative relations remain Conservative. Managed resource accounting retains its [documented scope](Managed-Resources.md); neither a numerical bound nor the Python model establishes a product RSS hard bound.

## Limitations and caller handling

Atom collection is limited to managed CPU Value dependency plans and the configured observation, work and root-capacity limits. Treat each contained failed outcome as a scoped observation; ordinary `execute` remains fail-fast. Quality evidence describes numerical estimates and remains separate from dependency completeness.
