---
spec_schema_version: 1
id: NUM-common
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM: specification and execution baseline

This contract is explicitly inherited by the NUM-01..NUM-15 and CRV-02 files
in this directory, including their family contracts. An individual operation's
explicit rule takes precedence over its family and this baseline. Related-file
links alone do not import that other operation's ports or numerical semantics.
The [operator template](../../00-foundation/spec-template.md) and
[foundation contracts](../../00-foundation/contracts.md) define the document
requirements and current execution boundary. This file fills shared requirements;
it does not change the maintainer's selected formulas or dependency exceptions.

## Status, provenance and registration

Front matter separates specification status, document maturity and implementation
status. D1_draft identifies a draft of the D1 mathematical scope; it is neither
Accepted nor an implementation gate. repository_branch/repository_commit record
the source-inspection baseline, not a claim that the new draft files existed in
that commit. Shared contracts register no operation; the primitive specifications and
[implementation table](../implementation.md) record the maintained runtime and
its validation. Existing unsuffixed implementations are only the explicitly
linked legacy subsets.

The proposed_operation_keys lists identify the maintained default-registry
names; specification acceptance remains Proposed. Each primitive has three
independently registered CPU entries: strict, Apple Silicon CPU
accelerated, and x86-64 CPU accelerated. No implicit mode dispatcher, unsuffixed
alias, GPU path or new runtime dtype is defined. Strict is portable across the
CPU targets actually supported by a conforming build. Accelerated keys require
their named CPU target and a verified ISA path; an incompatible target fails
BackendUnavailable. Selection and diagnostics must record the concrete library,
revision, build options, OS/architecture and ISA path. Using Clang alone selects
none of these mathematical guarantees or library dependencies.

Each spec's explicit numerical quality remains authoritative: raw copies preserve
bits; correctly rounded formulas round at their declared boundaries; only the
listed approximate functions receive their specified ULP allowance. Finite
rounding defaults to nearest/ties-to-even with gradual underflow. Preserve the
caller's floating environment, including prior flags, without global changes.
When an approximate path needs strict fallback, record actual per-node fallback
counts and reasons through execution diagnostics, not an invented data output.
Resource, cancellation and upstream failure must not be hidden by fallback.

## Interface and descriptor conventions

All listed ports are Value ports, required unless explicitly stated otherwise.
Input and output order is their listed order; single-output specifications use
values unless explicitly named otherwise. Each file supplies the shape, dtype
and static parameters or names the family from which it inherits them. Rank is
1..8, extents are positive and generic arrays have at most 2^40 logical elements;
the smaller count/K limits in generator specs take precedence. Shape products
and indexing use checked integer arithmetic. Outputs have empty facets and no
implicit physical units, colors or alpha roles. Typed input validation is retained
only under the actual-read obligations described by the individual contract.

Static parameters are supplied explicitly in direct WorkflowDocument nodes;
authoring defaults are written by constructors. An empty parameter set means no
static operation parameters. Reject unknown, missing or wrongly typed parameters
before sample reads. Canonical decimal list Strings use ASCII base-10 entries
separated by single commas, without whitespace, plus signs, leading zeros,
empty entries or a trailing comma. An entry is 0 or starts with 1..9; shape/count
entries must be positive, axis entries may be zero. Constructors serialize this
form; direct noncanonical input is rejected. Axis sets are sorted increasingly
by constructors; permutations and shapes retain their meaningful list order.
This grammar does not apply to expression source text or coefficient identifiers.

Compile/preflight validates all declared input descriptors, including unused
edges. Output descriptors depend only on input descriptors and declared static
parameters; no numeric producer is executed to infer a descriptor. A scalar
or an axis array carrying runtime values is still Data/Control, not descriptor
metadata. A changed shape/dtype/parameter requires inference and plan validation.
Per-node metadata specialization implements these relations before planning.
The primitive registrations and public workflow examples exercise the resolved
descriptors, including explicit Result schema specialization where specified.

## Demand, errors and returned observations

Q denotes requested logical output coordinates, independent of physical stride.
Individual files define exact per-port Data and Control sets and add recognized
typed Validation closures separately. Descriptor demand covers the static input
metadata needed by compilation. Empty Q introduces no runtime payload reads.
Dynamic controls retain witnesses even when they do not change returned values.
The operator must not enlarge its support to a bounding gap. Required upstream
execution may have its own indivisible or Whole support; these specifications
do not promise suppression of failures inside genuinely required upstream work.

The following default mapping applies where an individual file has not supplied
a more specific existing Status. Names follow the current
[public status types](../../../../include/photospider/core/status.hpp).

| Failure | Phase | Code / reason | Attribution |
| --- | --- | --- | --- |
| Missing, unknown or malformed static parameter; invalid axis/range/shape parameter | Compile or direct preflight | InvalidArgument / InvalidDomain | Schema origin; no invented runtime atom |
| Unsupported port dtype, rank, shape relation or output descriptor | Compile or direct preflight | TypeMismatch / None | Schema origin |
| Invalid dynamic bounds, control, index or step | Evaluation | InvalidArgument / InvalidDomain | Domain origin, affected output observation |
| Integer final overflow | Evaluation | OperationFailed / ArithmeticOverflow | Domain origin, actual output atom |
| Generator finite-only numerical failure | Evaluation | Individual generator's code/reason | Actual demanded value or axis observation |
| Unsupported accelerated target | Capability check | BackendUnavailable / None | Backend origin |
| Admitted capacity, work or stage exhausted | Admission or execution | ResourceExhausted / CapacityLimit, WorkLimit or StageLimit | Preserve host resource scope |
| Cancellation, stale input, typed validation or upstream failure | Respective host phase | Preserve original Status | Preserve original origin, scope and source identity |

Diagnostics identify the operation/output and global coordinate, plus the offending
port/index/value bits when relevant. Tags such as InvalidBounds or ViewUnavailable
are bounded human diagnostics, not new FailureReason enumerators or branchable
API fields. Metadata errors need not fabricate an atom key. For runtime local
numeric/control failures use Atom attribution for each dependent observation;
do not broaden a shared-input error into a whole-output ValidationDomain that
revokes already completed or independent observations. In particular, invalid
integrate_1d step affects positive indices only, and prefix_sum integer overflow
belongs only to the requested overflowing prefix. Existing typed validation
domains retain their own scope. Ordinary execute is fail-fast; execute_atoms
isolation requires the host's actual eligible Atomic/PerAtomOutcome plan.

A failed observation publishes no partial Value or successful certificate.
Independently completed observations retain their terminal outcomes. Dense
results cover requested global coordinates with owned packed fragments. Explicit
view policies may retain source owners or publish broader valid constant coverage
only as stated by that operation. Missing coverage is not an implicit zero.
All backing is immutable, survives context destruction while owners remain, and
is released at the final owner; unpublished allocations are released on failure.

## Resource and acceptance evidence

Per-operation complexity and scratch formulas supplement host accounting.
Reserve actual output, scratch, index/limb, metadata and retained-owner capacities
before allocation, including old/new growth overlap and simultaneously retained
source ancestry. Charge required validation and each committed work/stage unit;
failure does not refund committed work. A view's small output payload does not
bound its retained source allocation. Cache-off preserves active ownership.
No implicit disk spill, private unbudgeted workers or RSS upper bound is promised.
Each algorithm's stated cancellation interval also applies inside long exact
arithmetic, sorting or refinement; a native call must have bounded work or a
cooperative replacement. Exhaustion fails rather than weakening accuracy.

Each primitive's fixture supplies purpose and a minimal public workflow.
Implementation acceptance supplies its actual target/run command,
bindings, named outputs and inspected results through Compiler/ExecutionContext.
Keep a separate independent integer/rational, bit-pattern or directed-precision
oracle, and compare exact bits or the explicitly allowed ULP rule. Verify required
reads and dirty support separately from numeric equality. Exercise only the
relevant shape, special-value, layout, lifetime, low-budget and cancellation cases
listed in the primitive/family contracts. Maintained commands, independent
oracles and actual results are linked from the individual implementation sections
and the [public numeric workflows](../../../../examples/numeric_workflow/README.md).

Performance evidence records an analytic fixture and a declared representative
large shape within the operation's limits, dtype, requested region, backend and
hardware, worker count, cache state, repetition count, median/tail elapsed time,
output/scratch/retained managed peaks and quality/fallback results. Scans/reducers
also report actual source elements processed; sparse reads report their support
size. Hardware throughput thresholds and speedups are not yet measured or
promised. Implementation delivery supplies measured results and accounting
exclusions instead of treating asymptotic complexity as a benchmark. The
[native category driver](../../../../examples/numeric_workflow/README.md#native-category-timing-and-accounting)
records the remaining 18 clusters; expression, unary/binary, interpolation,
function sampling and inverse/lowpass measurements are linked from the same
workflow README. Each measurement declares its representative operation and
shape; it is not a complete parameter or platform matrix.

No unresolved user-facing semantic choice is recorded by this baseline. Concrete
backend selection, metadata specialization, diagnostics and runnable fixtures are
implemented and covered by the category delivery record. The 330 specified
primitive keys were queried successfully through the default public registry.
Implementation completion does not promote Proposed specifications to Accepted.
External compatibility claims remain limited to the cited source/version evidence
in individual files.
