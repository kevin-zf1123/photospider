---
spec_schema_version: 1
id: CRV-01D
parent_id: CRV-01
function: interpolate_pchip_multi
operation_family: curve.interpolate_pchip_multi
proposed_operation_keys:
  - curve.interpolate_pchip_multi_strict
  - curve.interpolate_pchip_multi_accelerated_apple_silicon
  - curve.interpolate_pchip_multi_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-01D: interpolate_pchip_multi

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) and the explicitly named
single-function contract below for all unchanged execution and acceptance rules.
Clarification is complete; the target remains Proposed and its implementation
status is recorded below.

## Confirmed multi-function interface

Use separate dynamic inputs x[K], y[K,C], query[N] and output values[N,C].
All C functions share x knots and query positions; interpolate each y column
independently using the pchip mathematical function. Preserve query order and
the function-column axis, including C=1. Output has no inferred RGB, color,
alpha or other image semantics. This is independent of the single-function
[CRV-01B operation](CRV-01B_interpolate_pchip.md); do not overload one
name with rank-1 and rank-2 y/output forms.

## Confirmed scalar inheritance and per-column demand

Each output values[i,c] has the same formula, dtype rules, finite-value policy,
domain policy, precision and zero handling as CRV-01B at query[i] using the
column y[:,c]. The static dtype and out_of_domain choices apply to the entire
node. Each port has one dtype, so y columns share that array's dtype; x, y and
query may independently use Float32/Float64. Outputs select Float32/Float64,
default Float64, with empty facets.

For any nonempty Q, all x knots are globally read and validated. Query Control
support is the row projection of Q, deduplicated across requested columns.
The y Data set is the exact union of the scalar method's required knot indices
paired with each actually requested column. Unrequested columns and remote
unselected y entries have no numeric demand or finite validation. Exact knot and
clamp paths read only y[j,c], and failed y in another column does not fail this
coordinate. Retain recognized typed Validation closure separately.

Errors are attributed to each dependent output Atom (i,c), preserving shared
x/query control effects and column-local y failures. Host upstream support,
eligible atom execution and fail-fast behavior retain their existing boundaries.

## Shape, parameters and inference

Require 2<=K<=65536, N>=1, C>=1, K*C<=2^40 and N*C<=2^40. No separate
small column limit applies. Check both products without overflow. x/query are
rank-1 and y/values are rank-2, with y.shape[0]=x.shape[0]. Infer output shape
[query.shape[0],y.shape[1]] and the static output dtype before reading payload.
All three input ports are required, in x/y/query order; values is the sole output.
Required String dtype is float32/float64 and out_of_domain is
reject/clamp/linear_extrapolate. Constructors write float64/reject respectively;
direct nodes specify both. There is no per-column parameter list or method mode.

Reject missing ports, rank/dtype/length mismatch and shape/product violations at
compile/preflight. Output metadata remains independent of numeric query order
and layout choices. Scalar and vector forms are not implicitly interconverted.

## Mapping, invalidation and ownership

For output Q, deduplicate its row indices for query reads and perform the scalar
segment lookup once per needed row when useful. Do not expand an irregular Q
into all columns for each row. Numeric evaluations use only the demanded (i,c)
pairs, with method-specific endpoint/stencil y coordinates in column c.
Changed x invalidates observations retaining global topology. A changed query[i]
invalidates the requested row's dependent outputs across columns; changed y[j,c]
invalidates only same-column outputs whose retained scalar stencil contains j.
Typed Validation support and its dirty effects remain explicit and separate.

Read arbitrary legal immutable strides, offsets and unaligned inputs, including
zero/negative strides along either y axis. Column independence is logical; it
cannot assume contiguous y columns. Return owned packed output fragments with
correct global rank-2 Region/storage origins, never writable aliases, implicit
zero gaps or a forced complete N*C allocation. Results survive context destruction
until final owner release; unpublished allocations are released on failure.
Cache keys include source/profile/static parameters and retained dependency
witnesses. Cache-off and changes to request partition do not change values.

## Algorithm and resources

Reuse the scalar mathematical evaluator per requested column. A shared bounded
x index and per-query segment classification may be reused across columns;
slope caches, where relevant, are keyed by column and exact source support.
Do not precompute all columns' coefficients for a partial-column request.
For M requested output cells and P distinct requested rows, indexed lookup work
is O(K+P log K), plus O(M) scalar evaluations and their exact arithmetic or
certification/refinement cost. Output payload is b*M. Charge index/segment/set
metadata, per-column active state, every arithmetic limb, source owners/windows,
validation and output fragments to host capacity/work/stage budgets.

The scalar cancellation intervals apply to x/query lookup and column evaluation;
large C does not permit a long uninterruptible inner loop. Poll extended arithmetic
and publication as well. Platform compatibility, actual fallback reporting and
finite-output errors follow the scalar operation. There is no unbudgeted worker
pool, disk spill or whole-grid intermediate. Each platform key has an independent
identity, and a failed observation publishes no partial successful value.

## Acceptance and implementation status


- [Family specification](CRV-01_interpolate.md).
- [Operator template](../../00-foundation/spec-template.md).

Conceptual public fixture: x=[0,1,2], y=[[0,4],[1,3],[4,0]],
query=[0.5,1.5] -> strict values=[[0.3125,3.6875],[2.1875,1.8125]].
The second column is 4 minus the first function. Validate accelerated ULP,
per-column shape/monotonicity, exact node/zero behavior and strict fallback
against the corresponding independent exact PCHIP oracle.

Compare every selected column with the matching single-function operation,
including mixed input dtype, both destinations, C=1 without squeezing, very
large logical shapes served sparsely, all domain policies and signed zeros.
Read-witness fixtures request one column while other columns contain invalid y;
only required typed-validation closure may extend the numeric support. A bad
shared query or x still affects every dependent selected column. Check ordinary
fail-fast and eligible per-atom success/failure isolation separately.

Deliver public WorkflowDocument examples with x/y/query bindings and named values
through Compiler/ExecutionContext, with actual build/run commands in the
maintained workflow. Apply scalar stride, cache, dirty, resource,
cancellation and post-context owner lifetime checks to the rank-2 output.
Measured native workloads and their limits are recorded in the implementation notes.

## Maintained implementation and validation

`plugins/ops/01-numeric/curve_interpolation.cpp` implements these three profile
keys. The public `photospider/numeric/curves.hpp` constructor is
`interpolate_pchip_multi_node`. Strict and Float64 outputs use exact rational evaluation and one final
destination rounding. Accelerated Float32 evaluation accepts only enclosures
whose endpoints round to the same Float32 result, preserving monotonicity;
unresolved cases use exact fallback. Exact cross products identify collinear
PCHIP stencils and reduce them to the linear formula. Global x validation, requested query rows and the
local y stencil follow the demand contract above.

The [family implementation record](CRV-01_interpolate.md#maintained-implementation-and-validation)
contains the shared arithmetic/resource details and actual platform acceptance.
See [the editable workflow](../../../../examples/numeric_workflow/README.md#explicit-query-curves-crv-01)
for construction, explicit work budgets, commands and checked expected results.
The manual target is excluded from default builds and CTest/integration testing.
Specification status remains Proposed.
