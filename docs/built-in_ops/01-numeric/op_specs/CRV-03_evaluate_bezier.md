---
spec_schema_version: 1
id: CRV-03
function: evaluate_bezier
operation_family: curve.evaluate_bezier
proposed_operation_keys:
  - curve.evaluate_bezier_strict
  - curve.evaluate_bezier_accelerated_apple_silicon
  - curve.evaluate_bezier_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
verification_status: focused_result_manual_and_installed_consumer
clarification_status: complete
repository_branch: ops-specs
repository_commit: current working tree
---

# CRV-03: evaluate_bezier

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for status, registry/profile
identity, platform availability, floating environment, resource/error handling
and acceptance. The per-component curve rules below take precedence. Completed
clarification does not change the Proposed status or implementation boundary.

## Confirmed scope

Evaluate quadratic or cubic parametric Bezier curves directly at dynamic
parameters, returning an array of D-component values. Only degree 2 and 3 are
included in this clarification round; Hermite and B-spline are not part of this
CRV-03 target. Individual coordinate components may turn back or repeat.

The mathematical operation evaluates B(t); it does not solve Bx(t)=x as
[CRV-02](CRV-02_sample_bezier_function.md) does. Query parameters are explicit
input data; no arc-length interpretation is implied. The three explicit profile keys and public `evaluate_bezier_node` constructor
are implemented; Proposed continues to describe specification acceptance status.

## Confirmed control representation

Use dynamic anchors[K,D] and relative handles[K-1,degree-1,D], with static
degree equal to 2 or 3 for every segment in a node. Adjacent segments share an
anchor. For segment j, quadratic control points are anchors[j],
anchors[j]+handles[j,0], anchors[j+1]. Cubic control points are anchors[j],
anchors[j]+handles[j,0], anchors[j+1]+handles[j,1], anchors[j+1].
No coordinate is required to be monotone and handles are not automatically
aligned. Finite arithmetic for reconstructing controls is defined below.

## Confirmed query representation

Queries use separate dynamic segment_indices[N] (Int64) and t[N] (floating).
Each row names a zero-based segment and its local parameter in [0,1]. Evaluate
that segment at the supplied t; no global-u decomposition or arc-length mapping
is performed. For example, segment_indices[i]=2 and t[i]=0.25 selects the third
segment at local t=1/4. Output values[N,D] preserves query row order.

## Confirmed dimensions and size limits

Require 2<=K<=65536 and D>=1. Anchors, handles and output each have a checked
logical element count at most 2^40: K*D, (K-1)*(degree-1)*D and N*D respectively.
D has no separate small-vector bound. D=1 retains a component axis. Components
are generic numeric vector entries; no geometry, RGB/alpha or physical units
are inferred merely from D. Query count N is positive and descriptor-defined.

## Confirmed types and finite-value policy

Anchors, handles and t independently accept Float32/Float64 and may mix dtypes.
Segment indices is always Int64. Output values selects Float32/Float64 through
a static dtype parameter, default Float64. Every mathematically used control component
and all t values must be finite, and each published result must be finite. Negative
and greater-than-one control values are valid; no value clipping is implicit.
Destination overflow and nonfinite demanded data fail the Run.

## Confirmed requested-component support

All inputs are Results containing one tensor member under any schema id/version
and member key; `sample_shape()` supplies the complete dimensions, including
batch axes. The Whole Result program requests every input with Data, Validation
and Descriptor (role 13), then validates every segment_indices/t row before any
component arithmetic. It evaluates all N*D output cells and publishes an
immutable dense Result using schema `photospider.tensor` v1 / member `samples`,
shape [N,D], selected dtype and empty facets. Output association records current
source ObjectIds. A nonempty local request returns full certified coverage in
global sample coordinates; coordinates are not rebased to the request. Empty
produces empty coverage and does not poll a failing handle producer; static
metadata and remaining execution stages still apply. For nonempty work,
upstream handle failures propagate before endpoint shortcuts or invalid-query
arithmetic.

Mathematical selection is unchanged: t=0/1 reads only the selected anchor
component for the polynomial; an interior uses both anchors and all relative
handles of that segment and component. Generic numeric data outside every
evaluated stencil is not additionally finite-checked. Complete typed and
upstream validation still covers unused active inputs. A typed RGBA handle
Result with spatial layout and channel axis 2 can fail validation on an invalid
alpha value. Whole execution computes all D columns even when the query requests
only one component, so that sparse request still evaluates the alpha component.
At t=0 the endpoint polynomial skips handles, while complete typed validation
still checks the handle Result and can reject that alpha value. All rows and
components are evaluated, so a bad index/t or evaluated component can fail the
Run. No partial successful output is published. There is no global mathematical
topology/monotonicity scan.

Any input edit invalidates recorded output demand. Cache identity includes all
input versions, profile, metadata and parameters. Replacing segment/t sources
retains the compiled static preparation. Repeating a completed demand returns
the same Result object; an equivalent fresh four-source binding can reuse the
content cache and refresh all four source associations. Output name, dtype,
rank-2 shape (including D=1) and empty facets are unchanged. Input
zero/negative strides, offsets and unaligned storage remain legal. Output
storage survives its context; the output owns its published payload, so source
backings can retire. An authorized read window shares the output owner and can
outlive the Result.

## Confirmed reconstruction and numerical versions

Widen each demanded Float32 input exactly to Float64. Reconstruct each demanded
absolute handle control component by correctly rounding anchor-plus-offset to
Float64, nearest/ties-to-even. These reconstructed controls define the curve,
consistently with CRV-02. Raw anchor controls preserve their widened bits.

Strict evaluates the mathematical Bezier polynomial of those controls at the
exact supplied t, then correctly rounds once directly to output Float32/Float64.
The two independently named CPU accelerated versions allow at most four final
FP32-scaled representable steps from strict. At t=0/1, all versions correctly
convert the selected anchor directly and preserve its zero sign. Intermediate
de Casteljau or polynomial roundings do not define the strict reference.

For an interior t with exact zero polynomial result, return -0 only when every
reconstructed control component for that requested component is -0. Otherwise
return +0. A nonzero exact result rounded to zero retains its mathematical sign.
Accelerated versions must match these zero classifications and signs exactly.

All accelerated component values must also stay between the smallest and largest
control components correctly converted to output dtype. Overflowed conversions
used only for those bounds act as extended infinities, not failures of another
finite requested value. The selected reconstructed Float64 controls themselves
must be finite; an anchor-plus-offset reconstruction overflow fails its demanded
interior component, even if an abstract unbounded-control curve could cancel.
Endpoint shortcuts never perform unneeded handle reconstruction.

At the same demanded inputs, accelerated and strict share finite/nonfinite
success classification and zero signs; the four-ULP allowance applies to nonzero
finite results. If a fast path cannot guarantee error, range and special cases,
fall back to strict. Per-value Whole counters are unavailable. Work/capacity exhaustion
fails explicitly. Results for a fixed profile/input/query do not depend on the
request partition, neighboring queries, thread order or cache state.

## Complete interface and polynomial

| Ordered input | Descriptor |
| --- | --- |
| 0 anchors | Float32/Float64 [K,D] |
| 1 handles | Float32/Float64 [K-1,degree-1,D] |
| 2 segment_indices | Int64 [N] |
| 3 t | Float32/Float64 [N] |

All ports are required Result inputs with one tensor member under any schema
id/version/key. Output port `values` is a Result using schema
`photospider.tensor` v1 / member `samples`, shape [N,D], selected dtype and empty
facets. Batch axes are included in the complete `sample_shape()`. Static degree is required Int64 2 or 3; static dtype is
required String float32/float64, with constructor default float64. Direct nodes
supply both. No output axis, extrapolation, automatic segment selection, squeeze
or implicit dtype conversion from integers is provided. Shape/dtype inference
uses descriptors and these static parameters, without numeric input execution.

Require 0<=segment_indices[i]<K-1 and finite 0<=t[i]<=1 for all rows.
Invalid indices and t fail, without clipping, wrapping or extrapolation. t=-0
is the t=0 endpoint. Query rows may be unordered or repeated. Adjacent segments
share their mathematical endpoint but may have unrelated tangents; corners and
coincident/degenerate anchors are valid. No smoothing or monotonicity is imposed.

For demanded segment j and component c, reconstruct scalar controls P[0..d]
as specified above. For 0<t<1 the exact mathematical value is

    B_c(t) = sum_{r=0..d} binomial(d,r)*(1-t)^(d-r)*t^r*P[r]

The strict result is direct RN_dtype(B_c(t)), with the explicit zero rule.
de Casteljau is an implementation option, but sequentially rounded blends do
not define this strict polynomial. Endpoint conversion and every interior
control reconstruction observe their separate rounding boundaries.

## Demand, dirty mapping and returned ownership

For a nonempty request Q, the operation returns complete certified [N,D]
coverage in global sample coordinates while dependency and dirty mappings retain
Q. Input version, profile, metadata and parameter identity participate in cache
validation; a source edit invalidates the corresponding recorded demand. The
manual fixture confirms replacement segment and t Results reuse the static
PreparedOperation and refreshes all four source ObjectIds when a fresh equivalent
bundle reuses the content cache.

The immutable dense output Result owns its packed payload independently of the
four source backings and execution context. A previously acquired authorized
read window shares that output owner and remains readable after releasing the
Result. Releasing the final window releases the output Payload and all Root
allocations. The Result schema and selected dtype remain stable across cache
rebinds.

## Algorithm, resources and errors

The callback retains one row classification and fixed arithmetic workspace,
independent of N and D. It validates all rows, then reclassifies each row once
for all columns. Work is O(N+N*D*degree), plus exact arithmetic, complete input
collection and typed validation. Complete output costs b*N*D bytes. Admit that
output and fixed workspace even for one requested cell, together with collected
inputs/retained owners and metadata. Giant broadcast inputs/output can therefore
fail a small payload budget. No per-cell dependency certificates or full
coefficient table is retained.

Use the host worker and resource ledger; poll cancellation on reads, row controls,
inside exact arithmetic and before publication. Work/capacity failures preserve
ResourceExhausted and release unpublished state/output. Numeric failures have Run
scope, identifying offending port/index where available. Invalid segment/t is
InvalidArgument/InvalidDomain; used nonfinite controls are OperationFailed/
InvalidDomain; actual RN64 reconstruction or final conversion overflow is
OperationFailed/ArithmeticOverflow. Typed, upstream, stale, backend and
cancellation errors preserve their categories. Whole numeric counters are not
available; zero counters must not be interpreted as zero arithmetic/fallbacks.
Public constant-node composition with D=2^39 and N=2^40 requests one cell but
requires full output capacity and fails at the parametric node with
ResourceExhausted/CapacityLimit. These cases verify the complete-output budget
rule, not successful numerical execution at either physical maximum shape.

## Acceptance and implementation boundary

Quadratic fixture: anchors=[[0,0],[2,0]], handles=[[[1,2]]],
segment_indices=[0,0,0], t=[0,0.5,1] -> values=[[0,0],[1,1],[2,0]].
Cubic fixture: anchors=[[0,0],[3,0]], handles=[[[1,3],[-1,3]]],
segment_indices=[0], t=[0.5] -> [[1.5,2.25]]. Use independent exact Bernstein
polynomials after the separately required RN64 reconstruction, not the production
de Casteljau helper, as oracle. Strict compares destination bits; accelerated
checks ULP, converted-control bounds, zero classification and strict fallback where needed.

Test linear/constant degeneracies, loops/reversals, multiple segments, shared
endpoints and corners, repeated/unsorted rows, D=1 without squeezing and large
D with sparse component requests. Include mixed dtypes, RN64 control midpoints,
actual reconstruction overflow, final cancellation, subnormals and signed zeros.
Mathematical endpoint evaluation ignores unused generic handles/opposite anchor,
but full collection and typed validation still apply. An unrequested bad query
or evaluated component fails the complete Run. Verify complete source support,
whole invalidation, all-port layouts/fenv, Empty/schema, active cancellation and
work/output/workspace rejection. Acceptance also requires public constant-node
composition cases showing that 2^39-column and 2^40-row sparse requests still
require the complete payload budget and fail when it is insufficient. The
Result manual workflow exercises both rejection cases under Strict and Apple
profiles; they do not establish successful numerical execution at either
maximum shape.

The public Result constructor is exercised by `test_numeric_parametric_result`
and `installed_numeric_parametric_result`, which builds the same manual fixture
against the installed package. Its source `Value` objects supply immutable
storage referenced by source Results and charged as Referenced; output values,
bindings, and reads use Results. Coverage includes both profiles, Float32/Float64,
quadratic/cubic fixtures, all 16 combinations of reversed/unaligned inputs,
zero-stride sources, caller and worker fenv, nine invalid static metadata cases, typed Whole
validation, unused generic values, query/producer error ordering, endpoint
selection, overflow, Empty, current association, demand dirty mapping, static
preparation, exact-work cancellation, WorkLimit and complete Root release.
Strict and Apple each passed all six manual groups and all 1428 independent
Fraction oracle cases bit-for-bit. The focused root CTest selection passed
`test_numeric_result_math` and `test_numeric_parametric_result` 2/2 in 4.67
seconds (4.50 seconds and 0.17 seconds respectively). The installed 0.32.0
package consumer passed `installed_numeric_parametric_result` 1/1 under Strict
in 0.17 seconds; its direct Apple invocation passed all six groups. The separate
`test_numeric_result_math` integration target
continues to cover 22 Fraction golden words across CRV-02 function sampling and
CRV-03 parametric evaluation; these integration cases do not replace the manual
fixture or its installed consumer. Exact commands and current validation results
are recorded in the [numeric workflow README](../../../../examples/numeric_workflow/README.md#parametric-bezier-evaluation-crv-03).
x86 and successful physical maximum-shape numerical execution were not tested.

Production retains RN64 reconstruction and exact integer power-Horner with one
final rounding. All current profiles use the same exact numerical calculation;
NEON/AVX2 supply integer helpers. ExactPolynomial's fixed arena is reused with a
5329-bit numerator bound for degree<=3 and t in [0,1]. No inverse or global
mathematical topology pass is performed. Whole retains one row and fixed scratch
instead of per-output associations. See implementation notes for public/core
performance and its limits.

Build/run commands and editable use are maintained in
[the numeric workflow README](../../../../examples/numeric_workflow/README.md#parametric-bezier-evaluation-crv-03).
The focused Result test command and coverage are in the numeric workflow README.
See [implementation notes](../math-implementation.md#crv-03-parametric-bezier-evaluation)
for unchanged arithmetic bounds and historical validation scope.

- [Curve category](../curves.md).
- [Operator template](../../00-foundation/spec-template.md).
- [Foundation execution contract](../../00-foundation/contracts.md).
