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
clarification_status: complete
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
default Float64, with no attached domain description.

Execution, physical read closure, caller preconditions, resource admission and ownership follow the [family fixed execution contract](CRV-01_interpolate.md#fixed-execution-resources-and-caller-preconditions). Each selected output tile reads all x, all y rows for its selected columns, and its query row range through the corresponding input grids. Only the tile's global output coordinates are evaluated. Mathematical stencil selection remains defined by this member specification.

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

## Fixed mapping and ownership

Execution, physical read closure, caller preconditions, resource admission and ownership follow the [family fixed execution contract](CRV-01_interpolate.md#fixed-execution-resources-and-caller-preconditions). Each selected output tile reads all x, all y rows for its selected columns, and its query row range through the corresponding input grids. Only the tile's global output coordinates are evaluated. Mathematical stencil selection remains defined by this member specification.

Classification runs once per query row within each tile and is reused across that tile's columns. Input batch and atomic axes are respected by each source grid. The output column axis remains ordinary, including C=1.

## Algorithm and resources

The [family resource contract](CRV-01_interpolate.md#fixed-execution-resources-and-caller-preconditions) applies. Each task has one preallocated `ExactCurve` arena and selected output storage. Direct binary search replaces a copied knot array; classification is shared by columns within the task. Resources scale with declared input tiles, selected outputs and granted-lane scratch, rather than the complete logical output. Capacity checks and cancellation preserve their operational categories.

## Acceptance and implementation status

- [Family specification](CRV-01_interpolate.md).
- [Operator template](../../00-foundation/spec-template.md).

Conceptual public fixture: x=[0,1,2], y=[[0,4],[1,3],[4,0]],
query=[0.5,1.5] -> strict values=[[0.3125,3.6875],[2.1875,1.8125]].
The second column is 4 minus the first function. Validate accelerated ULP,
per-column shape/monotonicity, exact node/zero behavior and strict fallback
against the corresponding independent exact PCHIP oracle.

Compare every selected column with the matching single-function operation,
including mixed input dtype, both destinations, C=1 without squeezing, all
domain policies and signed zeros. Very large logical shapes with sparse demand must fit only their declared input tiles, selected output storage and fixed scratch. Inspect exact static read sets, including atomic source tile rounding. Verify that unselected query rows and columns do not enter arithmetic.

Deliver public WorkflowDocument examples with x/y/query bindings and named values
through Compiler/ExecutionContext, with actual build/run commands in the
maintained workflow. Apply scalar stride, rebinding, resource,
cancellation and post-context owner lifetime checks to the rank-2 output.
Measured native workloads and their limits are recorded in the implementation notes.

## Maintained implementation and validation

`plugins/ops/01-numeric/curve_interpolation.cpp` implements these three profile
keys. The public `photospider/ops/numeric/curves.hpp` constructor is
`interpolate_pchip_multi_node`. Strict and Float64 outputs use exact rational evaluation and one final
destination rounding. Accelerated Float32 evaluation accepts only enclosures
whose endpoints round to the same Float32 result, preserving monotonicity;
unresolved cases use exact fallback. Exact cross products identify collinear
PCHIP stencils and reduce them to the linear formula. Fixed task reads, selected output allocation and caller preconditions follow the family execution contract; mathematical y stencils remain unchanged.

The [family implementation record](CRV-01_interpolate.md#maintained-implementation-and-validation)
contains the shared arithmetic/resource details and current validation boundary.
This
operation's specification status remains Proposed.
