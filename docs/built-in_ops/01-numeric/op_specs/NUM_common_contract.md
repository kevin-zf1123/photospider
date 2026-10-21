---
spec_schema_version: 1
id: NUM-common
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM: specification and execution baseline

The [FMT explicit-configuration contract](../../02-format-color/op_specs/FMT_common_contract.md)
keeps domain interpretation outside Result/Tensor. Numeric operations receive
structure and samples; any color, alpha, sampling or LUT interpretation required
by an operation is a declared static parameter. Outputs do not propagate or
clear a facet list, because no such list exists. Numerical formulas, rounding
and strict/accelerated precision are unchanged by this ownership boundary.

## Static structure and unchecked premises

Compilation and static preparation infer and check schema identity/version,
member directories, dtype, rank, extents, axes, shape relations, parameter types
and syntax, and fixed read/write/scratch signatures. Static capacity arithmetic
is checked. Runtime payload structure (counts, offsets, indices, identities,
associations and canonical padding/encoding) and numerical/geometric conditions
are unchecked caller premises. A numerical parameter's presence at preparation
time does not turn its finite/range/order/positivity condition into a structural
check. Producers construct results according to the contract without a final
validation scan or publication gate; consumers trust those premises without
rescanning or comparing payloads.

Precision refinement, accelerated-path acceptance, candidate selection,
explicit clipping, error measurement and an explicitly requested quality gate
remain their named computations. A quality gate consumes the supplied outcome;
it does not revalidate the report or its association with other inputs. Resource,
backend, cancellation and actual execution failures retain their normal meaning.
Raw IEEE rules for NaN, infinities and signed zeros remain as specified. Outside
a mathematical/payload premise no diagnostic, successful result or recovery is
promised. This boundary applies throughout NUM/CRV, including SDK reports.

The [relative-coordinate scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
sets native CIELAB/CIELCh lightness to l=L*/100. NUM/CRV consumers now use
TensorDescription v4/v5 logical tuple parameters with that convention. The old
ColorArray public API and decoder are removed; no implicit L* compatibility
exists. This changes color interpretation and configuration identity, while
ordinary numeric formulas, precision and the frozen kernel remain unchanged.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

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
Accepted nor an implementation gate. Shared contracts register no operation; the primitive specifications and
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

The [accelerated contract](NUM_accelerated_contract.md) defines final FP32-scaled
accuracy for floating arithmetic throughout NUM and CRV. Copies, predicates,
indices and selected endpoints remain exact. Strict formulas retain their
specified rounding boundaries. Finite
rounding defaults to nearest/ties-to-even with gradual underflow. Preserve the
caller's floating environment, including prior flags, without global changes.
When an approximate path needs strict fallback, record actual per-node fallback
counts and reasons through execution diagnostics, not an invented data output.
Resource, cancellation and upstream failure must not be hidden by fallback.

## Interface and descriptor conventions

Runtime operation ports are Results, required unless explicitly stated otherwise.
A numeric input normally contains one tensor; individual contracts
define the allowed schema and any field-only reports. Tensor is backing storage.
Input and output order is their listed order; single-output specifications use
values unless explicitly named otherwise. Each file supplies the shape, dtype
and static parameters or names the family from which it inherits them. Rank is
1..8, extents are positive and generic arrays have at most 2^40 logical elements;
the smaller count/K limits in generator specs take precedence. Shape products
use checked integer arithmetic during static inference. Dynamic indices and
address-control values satisfy unchecked caller premises. Outputs have no
attached domain description or implicit physical units, colors or alpha roles.

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
Individual files define exact per-port Data and Control sets. There is no
implicit sample-validation support; structural tuple closure uses
atomic_trailing_axes. Descriptor demand covers the static input
metadata needed by compilation. Empty Q introduces no runtime payload reads.
Dynamic controls retain witnesses even when they do not change returned values.
The operator must not enlarge its support to a bounding gap. Required upstream
execution may have its own indivisible or complete-input support; these specifications
do not promise suppression of failures inside genuinely required upstream work.

The following default mapping applies where an individual file has not supplied
a more specific existing Status. Names follow the current
[public status types](../../../../include/photospider/core/status.hpp).

| Failure | Phase | Code / reason | Attribution |
| --- | --- | --- | --- |
| Missing, unknown or malformed static parameter; invalid structural axis/shape parameter | Compile or direct preflight | InvalidArgument / InvalidDomain | Schema origin; no invented runtime atom |
| Unsupported port dtype, rank, shape relation or output descriptor | Compile or direct preflight | TypeMismatch / None | Schema origin |
| Violated runtime structure or numerical/geometric premise | Outside the operator contract | No validation or diagnostic guarantee | Caller responsibility |
| Unsupported accelerated target | Capability check | BackendUnavailable / None | Backend origin |
| Admitted capacity, work or stage exhausted | Admission or execution | ResourceExhausted / CapacityLimit, WorkLimit or StageLimit | Preserve host resource scope |
| Cancellation, stale input or upstream failure | Respective host phase | Preserve original Status | Preserve original origin, scope and source identity |

Diagnostics identify the operation/output and global coordinate, plus the offending
port/index/value bits when relevant. Tags such as InvalidBounds or ViewUnavailable
are bounded human diagnostics, not new FailureReason enumerators or branchable
API fields. Metadata errors need not fabricate an atom key. Actual operational
failures retain their existing observation scope; do not add a validation domain
to revoke completed observations. Invalid dynamic steps, indices, bounds and
unrepresentable arithmetic results governed by caller premises have no required
error delivery. Ordinary execute remains fail-fast for actual failures.

A failed observation publishes no partial Tensor or successful certificate.
Independently completed observations retain their terminal outcomes. Dense
results cover requested global coordinates with owned packed fragments. Explicit
view policies may retain source owners or publish broader valid constant coverage
only as stated by that operation. Missing coverage is not an implicit zero.
All backing is immutable, survives context destruction while owners remain, and
is released at the final owner; unpublished allocations are released on failure.

## SDK numeric reports

`NumericDiagnostics` and its classification codec are declared in
`photospider/ops/numeric/diagnostics.hpp` and belong to `Photospider::ops_sdk`.
`register_numeric_diagnostics` registers its fixed 588-byte codec for explicit
`DiagnosticAccumulator` use. Fixed invocation ABI 8 has no report-submission
callback and does not automatically emit per-operation numeric reports. The
frozen accumulator retains its envelope, allocation and transactional merge
rules. The ops validation callback is a no-op: payload identity/counter
consistency and canonical encoding are caller premises. `read_numeric_report`
decodes an already supplied complete 588-byte payload without checking it;
`merge_numeric_diagnostics` computes totals and attribution, returning actual
counter-capacity exhaustion without mutating the destination.

```cpp
// payload is a complete SDK numeric report supplied under the caller premises.
auto decoded = ps::read_numeric_report(payload);
if (!decoded.ok()) return decoded.status();
auto status = ps::merge_numeric_diagnostics(&total, decoded.value());
if (!status.ok()) return status;
```

Reports describe actual arithmetic work when explicitly collected. Their values
do not certify sample domains, report association or a numerical accuracy bound.
Report bytes and callback addresses do not enter computation identity.

## Resource and acceptance evidence

Per-operation complexity and scratch formulas supplement host accounting.
Reserve actual output, scratch, index/limb, metadata and retained-owner capacities
before allocation, including old/new growth overlap and simultaneously retained
source ancestry. Charge structural preparation and each committed computational work/stage unit;
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
listed in the primitive/family contracts.

No unresolved user-facing semantic choice is recorded by this baseline. Concrete
backend selection, metadata specialization, diagnostics and runnable fixtures are
implemented and covered by the category delivery record. The 330 specified
primitive keys were queried successfully through the default public registry.
Implementation completion does not promote Proposed specifications to Accepted.
External compatibility claims remain limited to the cited source/version evidence
in individual files.
