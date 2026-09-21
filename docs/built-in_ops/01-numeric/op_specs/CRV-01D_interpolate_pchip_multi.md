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
implementation_base_commit: 3d35f5eb
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

For every nonempty request, one CPU Whole callback collects complete x, y and
query inputs, including recognized typed validation and upstream failures. It
validates all x knots and all query controls before y arithmetic, evaluates every query and every output column, and returns
one immutable dense output of shape [N] or [N,C]. Empty reads no payload; static
metadata validation still applies. Sparse demand restricts publication coverage,
but does not reduce input collection, computation or the complete output owner.

The mathematical stencil remains unchanged: an exact knot/clamp uses one y;
linear uses two endpoints; PCHIP uses its fixed local stencil. Generic y values
outside every evaluated stencil do not undergo an additional finite scan. Typed
validation and upstream execution cover complete inputs, including unused values.
All query rows and output columns are evaluated, so errors in unrequested rows
or columns can fail the run. Reject still performs full upstream collection.

Any input change invalidates the recorded output demand. Cache identity includes
complete input versions, profile, metadata and parameters. Numerical failures
have Run scope and publish no partial successful output; they do not provide
independent per-column Atom success. Input strides, offsets, zero strides and
negative strides remain legal. Packed output storage outlives the context.

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

Whole support, Run failure and ownership follow the scalar contract. Lookup
runs once per complete query row and is shared across all C columns. Per-column
formulas and numeric selection are unchanged. Every output column is computed;
any input edit invalidates recorded output demand. Arbitrary legal immutable
strides on either y axis remain supported. Output retains the rank-2 shape and
empty facets, including C=1.

## Algorithm and resources

Search/classification costs O(K+N log K), followed by N scalar evaluations
(or N*C for multi). Classification is shared across columns. The complete dense
output costs b*N (or b*N*C) bytes; reserve it even for one requested cell. Input
collection and retained owners also require admission. The callback declares its
fixed exact arithmetic workspace, and the host-accounted knot vector has 8*K
element bytes plus allocator/metadata overhead. K<=65536 bounds its elements at
524288 bytes. No full slope table is required.

Poll work/cancellation during reads, binary search, exact arithmetic and before
publication. Capacity and work exhaustion return ResourceExhausted, with failed
output/workspace released. A giant logical broadcast can therefore fail a small
payload budget even for sparse demand. Resource limits do not authorize weaker
arithmetic. Backend, typed, upstream, stale and cancellation failures retain their
categories. Numeric failures are OperationFailed/InvalidDomain or final-output
ArithmeticOverflow, with Run scope and offending port/index where available.

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
domain policies and signed zeros. Very large logical shapes with sparse demand
must still satisfy the complete-output budget. Read witnesses cover complete
inputs; invalid data in another evaluated column fails the Whole run. Test
both numeric failures and complete typed validation, including unused values.

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
PCHIP stencils and reduce them to the linear formula. Complete input collection, Run failures and complete-output allocation follow
the Whole contract above; mathematical y stencils remain unchanged.

The [family implementation record](CRV-01_interpolate.md#maintained-implementation-and-validation)
contains the shared arithmetic/resource details and actual platform acceptance.
See [the editable workflow](../../../../examples/numeric_workflow/README.md#explicit-query-curves-crv-01)
for construction, explicit work budgets, commands and checked expected results.
The manual target is excluded from default builds and CTest/integration testing.
Specification status remains Proposed.
