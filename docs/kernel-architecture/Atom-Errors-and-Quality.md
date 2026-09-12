# Atom errors and numerical quality

Phase A #319 implements these C++ contracts in package 0.10.0. The installed
[example](../../examples/atom_outcomes_workflow/README.md) runs real regional
sources, compiled DAG callbacks and owning sink observations.

## Coordinate batches

`OperationTraits::joint_contract = 2` accepts 1..64 distinct `AtomKey` members.
A key is the declaration-order output index plus a rank-1..8 logical observation
coordinate. Images use HW observation coordinates, not channel samples. Unused
coordinate slots are zero. Contract 1 remains the ABI 9 unique-output protocol.
The C++ outcome, progress, supply and pending-read APIs use the complete key.

Every poll supplies exactly the current Ready members. Each must return one
Success, Need, or failure. The host checks the entire envelope's membership,
payload metadata, failure scope and quality attachment before delivering any
reply. Unknown, duplicate, missing, Waiting or Terminal keys are protocol errors.
Need suspends one member until its exact authorized input coverage is supplied.
Each member terminates once; a local failure does not require sibling failure.
Callbacks cannot retain borrowed phases or block on upstream work. State and
returned values own their separately admitted storage.

Joint phases and their borrowed function objects occupy one host-admitted
buffer. Preparation charges each member's stage once, the callback runs once
under the floating-point environment, and validated replies retire members
sequentially. There is no recursive Session::poll stack per member. Direct hosts
can budget `member_phase_bytes()` in addition to shared/member scratch.
Checkpoint cache absence still supplies checked misses and pure block execution.

The CPU coordinator gathers up to 64 already demanded coordinates, including
coordinates of one output, and registers all returned input demands before
performing upstream reads. It deduplicates identical source transports; it does
not union distinct requests into a new semantic validation domain. Contract 2
never retries an enclosing group failure as singletons. The existing contract 1
optional admission fallback remains its separate behavior.

`ExecutionContext::execute_atoms(plan, bindings, requested, token, options)`
collects owning `AtomObservation` records for managed CPU Value dependency plans.
Named requested outputs must be Atomic/PerAtomOutcome. A request is bounded by
`maximum_atom_observations` (default and hard cap 65536), root capacity and work
limits. Empty demand returns no observations. Ordinary `execute` remains
fail-fast. Structured Result publication uses its existing CompleteBundle,
StablePrefix or IndependentChunks policy. Atom collection does not introduce
persistent execution state or a GPU batch backend.

## Failure identity and scope

`Status.code` retains the coarse error category. `reason` is a machine-readable
cause, and `detail` records origin and scope. Diagnostic strings are not branch
conditions. Unspecified fields are the legacy category-only projection.

- Atom identifies exactly one logical producer observation.
- ValidationDomain identifies a fixed semantic range. Contract 2 currently
  accepts the entire immutable output observation shape. It propagates a domain
  failure to covered Ready and Waiting members and rejects late revocation of
  prior covered semantic terminal outcomes. A poll's requested subset cannot
  define this domain.
- Association identifies the immutable Result ObjectId being validated before
  publication. Associated fields are checked before observers see that result.
- Group, Run and Waiter describe operational extent and do not invent a failed
  semantic value at an arbitrary coordinate.

The host binds `node_id` or `input_id` to the actual source of a failure.
Propagation retains that identity and the original atom. `AtomObservation`'s
output/key describe the requested consumer, so a downstream coordinate can
observe a failure caused by a different upstream coordinate. A transport read
failure remains Io/Group with its input declaration identity. Failure records
have no success Value or success dependency certificate.

Read, work and allocation service errors are sticky, including exceptions
caught by a callback. A callback cannot replace a prior service failure by
returning a value or another same-code status. A detected Protocol violation
survives later cancellation. Shared-result waiter cancellation remains local;
a previously recorded Protocol terminal remains observable. Invalid callback
membership or payload rejects the current envelope before its successes escape.

Failed ResultRefs, structured actors and shared waiters retain the first cause,
reason and scope in inline bounded diagnostic storage (up to 256 diagnostic bytes). The host
may fill missing producer identity/scope for that same cause; it does not change
an already specified upstream identity. Diagnostic-copy allocation failure
returns the fixed machine-readable fields with an empty diagnostic. These
inline bytes are charged with their owning managed runtime objects.

## Quality evidence

`QualityReport` is an immutable owning allocation created only by named factories.
Its evidence enum cannot be promoted by a caller-supplied flag. Measured finite
residuals carry no certified error bound. CertifiedBound is currently available
for the checked integer diagonal system factory only, with 1..4096 rows,
nonzero diagonal and magnitudes at most 2^26. Snapshot names are 1..256 printable
ASCII bytes. The report retains all exact integer proof rows.

For the stored system, let R=max|a_i*x_i-b_i| and m=min|a_i|. Products and
residuals are exact integers below 2^53. The infinity-norm solution error is at
most R/m; residual rounding error is zero. A positive binary64 quotient is
computed under nearest rounding and moved once toward positive infinity.
R=0 produces an exact zero bound. This proof does not cover arbitrary Float64
systems, residual-only stopping rules, other norms or relative error.

Contract 2 runtime attachments currently require a facet-free Float64 vector
whose dimension matches the report. A successful atom's actual estimate must
match the retained certified proof row exactly. A report for integer 2^24+1
therefore cannot certify an output rounded through Float32 to 2^24. A Need
cannot carry quality. Domain failures may carry Measured evidence, which follows
the original failure through downstream consumers; it does not describe a new
consumer estimate. Successful operation transformations do not implicitly
transfer another operation's report. Optional result cache retention skips
quality-bearing values; owning in-flight/result handles retain their reports.

Numerical evidence and dependency guarantees are independent. Conservative
relations remain Conservative. Managed resource accounting retains its
[documented scope](Managed-Resources.md); neither a numerical bound nor the
Python model establishes a product RSS hard bound.
