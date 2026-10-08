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
  Result<ResultRef> outcome;
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

`OperationTraits::joint_contract = 2` accepts 1..64 distinct `AtomKey` members. A key is the declaration-order output index plus a rank-1..8 logical observation coordinate. For structured Result tensors, the coordinate uses the remaining full-sample axes after tuple-channel and atomic trailing axes are grouped; batch axes remain coordinates. Unused coordinate slots are zero. Contract 1 groups distinct outputs; contract 2 identifies every member with a complete `AtomKey`. `ResultJointOutcome` and the Result joint callback phases use that key.

Every poll supplies exactly the current Ready members. Each must return one Success, Need, or failure. The host checks the entire envelope's membership, payload metadata, failure scope and quality attachment before delivering any reply. Unknown, duplicate, missing, Waiting or Terminal keys are protocol errors. Need suspends one member until its exact authorized input coverage is supplied. Each member terminates once; a local failure does not require sibling failure. Callbacks cannot retain borrowed phases or block on upstream work. State and returned values own their separately admitted storage.

Each `ResultJointContinuation::poll` receives the ready members and their borrowed phase services. The host validates the complete outcome set before exposing member replies, and borrowed phases expire when the callback returns. The coordinator registers returned Needs before performing upstream reads, charges shared workspace and Run/Root work, and retains the first failure. It does not merge distinct requests into a new semantic validation domain. Contract-2 group failures are terminal; contract 1 retains its separate optional admission fallback.

The CPU coordinator gathers up to 64 already demanded coordinates, including coordinates of one output, and registers all returned input demands before performing upstream reads. It deduplicates identical source transports; it does not union distinct requests into a new semantic validation domain. Contract 2 never retries an enclosing group failure as singletons. The existing contract 1 optional admission fallback remains its separate behavior.

`ExecutionContext::execute_atoms(plan, bindings, requested, token, options)` collects owning `AtomObservation` records from a managed CPU structured Result dependency plan. Each requested output must declare Atomic, PerAtomOutcome and joint contract 2. Before preparing actors or invoking callbacks, the coordinator validates all requested outputs and the combined observation count, bounded by `maximum_atom_observations` and a hard limit of 65,536. Empty demand returns no atom records and starts no producer.

For tensor outputs, `requested` footprints and operation Needs use the complete sample shape, with batch axes before cell axes. Tuple-channel and atomic trailing axes are grouped into each observation; remaining batch and cell coordinates form its `AtomKey`. Each record returns `Result<ResultRef>` so successful observations retain the actual Result and dependency evidence. A local Atom, ValidationDomain or Group failure remains attached to its observation, with an optional matching quality report. Protocol, Run, Waiter and call-level cancellation failures remain enclosing errors. Contract 2 can batch up to 64 one-observation queries, including different Q values for one output; disabling grouping retains the one-member joint callback, and group failure is not retried as singleton callbacks. This API has no GPU path. Other Value execution entry points remain separate and available.

### Direct structured Result joint contract 2

`OperationRegistry::start_result_joint` also accepts contract 2 for direct CPU Result continuations. Registration declares Atomic outputs with `PerAtomOutcome` delivery. A start has 1..64 distinct `AtomKey` members; several members may name one output when their coordinates differ, but each output uses one tensor slot. Direct registry callers drive the continuation themselves; `ExecutionContext::execute_atoms` uses the structured coordinator to supply Needs, validate publications and collect named observations.

`result_atom_key(query)` identifies the selected output and one logical observation. For a tensor query, the host closes tuple-channel and atomic trailing axes into one observation and omits those axes from the key. The remaining sample axes stay in order, including batch axes, and each must select one coordinate. Channel samples therefore are not separate atoms. `result_observation_domain(query)` returns the full fixed domain over those remaining axes, independent of the requested query subset. Collection-only outputs use the singleton key at coordinate zero and a singleton domain.

Each Ready member returns a `ResultJointOutcome` keyed by its `AtomKey`: a Result Need, complete publication, or typed failure. A Need carries no quality evidence. An Atom failure must name its exact key. A `ValidationDomain` failure must use Domain or Schema origin and match the output's complete fixed observation domain; it applies to matching Ready and Waiting members in the invocation. The host rejects a later domain failure that would revoke a previously semantic-terminal member.

Contract 2 can attach a `QualityReport` to a successful publication or a matching Domain failure. A successful estimate must be a generic Float64 vector with one element per report dimension; the host reads the member's estimate and checks certified rows against the actual value. Measured evidence requires a finite estimate but asserts no certified bound. A Measured report may accompany a Domain failure. The report's snapshot label is independent of the joint query's routing `snapshot_identity`.

The report must belong to the member's ResourceBudget and be owned by `phase.resources.allocator()` plus either its phase allocator or the shared workspace allocator. The continuation also checks that phase/member allocators share the Root's nonempty accounting domain; `BufferAllocator::same_owner` compares only that domain, not quotas or provenance, and does not authorize payload access. A read failure while checking an estimate preserves its original status. Cancellation clears the report and returns a local Cancelled outcome; a nonfinite estimate or certified-row mismatch sets `FailureReason::InvalidQuality`.

The direct C Result joint table also supports contract 2. It copies a typed Atom or ValidationDomain failure record with a bounded diagnostic; the host supplies producer identity and enforces the same exact-Atom and fixed-domain rules described above. Its shared poll services create Measured and IntegerDiagonal reports with epoch-scoped handles, and the host copies an attached owning report before the callback lease retires. Reports attach to successful publications or to Domain-origin Atom/ValidationDomain failures carrying Measured evidence. A report may be created in a poll that returns Need, but the Need outcome must not attach it; an unused report retires with the lease. Contract 1 does not carry quality evidence. The C singleton operation table still has no typed Atom or quality attachment.

Structured execution schedules eligible CPU Result queries through contract 1 or 2. Contract 2 groups up to 64 one-observation queries, including different queries of the same output. Each C2 member represents one output observation, but its input Tensor Needs may span multiple observations. If an input Need targets a computed C2 producer, the coordinator expands its requested samples into independent singleton-atom requests, up to `min(65,536, maximum_boxes)`, and schedules those requests in cohorts of up to 64. It supplies a piecewise `ResultTensorInput` backed by the original Results. Each piece retains its original Result owner, captured descriptor and granted samples; the coordinator creates neither an aggregate Result nor a new validation domain. This expansion does not narrow wider computed Tensor Needs on other paths. Disabling joint grouping still runs contract 2 through its required one-member joint callback; a contract-2 group failure is not retried through singleton callbacks. The Run ledger keys ValidationDomain failures and semantic-terminal observations by compiled producer semantic identity, output and tensor slot. Semantic aliases of the same compiled producer share this finality; failure details still identify the actual workflow node that failed. A cohort keeps one tensor slot per output, while queries for another slot are handled by a separate cohort. The ledger spans cohorts, completed-cache reuse and Actor retirement. A domain failure reaches matching Ready and Waiting members, and a later failure cannot revoke an earlier terminal observation. The coordinator allocates each query's payload-free failure and quality record before invoking callbacks, so recording a terminal outcome does not need a later allocation.

The compound capability authorizes only the union of its granted samples. `object_id()` reports a source identity for a singleton Result and zero when several original Results back the capability. Zero means there is no single Result identity; it authorizes neither payload reads nor a `result_descriptor` call. The phase association still records each real input port and source ObjectId. Acquired windows can combine authorized backing pieces without copying payload samples while retaining every original Result owner, including owners held through fields, associations and resources. A published view still needs one physically valid affine representation, so arbitrary partitions are not guaranteed to admit one view. This expansion is limited to tensor Needs; it does not aggregate object or field support. Descriptor-only Empty C2 input and GPU C2 joint execution are outside this path.

Shared producer entries retain quality with their publication or failure for waiters. Domain failure identity and evidence continue downstream; successful transformations do not inherit a source report. Quality-bearing results are excluded from completed-output content retention. Structured Result joint execution has no GPU path.

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

Atom collection is limited to managed CPU structured Result dependency plans and the configured observation, work and root-capacity limits. Treat each contained failed outcome as a scoped observation; ordinary `execute` remains fail-fast. Other Value execution entry points remain available separately. Quality evidence describes numerical estimates and remains separate from dependency completeness.
