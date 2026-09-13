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
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-03: evaluate_bezier

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
input data; no arc-length interpretation is implied. No runtime operation or
implementation result is claimed.

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
a static dtype parameter, default Float64. Every actually read control component
and t value must be finite, and each published result must be finite. Negative
and greater-than-one control values are valid; no value clipping is implicit.
Destination overflow and nonfinite demanded data fail the affected observation.

## Confirmed requested-component support

For requested output (i,c), first read and validate segment_indices[i] and t[i].
Only the selected segment's component c is required. At t=0 or t=1, read only
the corresponding anchor component; no handle or opposite endpoint is read.
Interior t reads the control components needed by that segment and component.
Other segments, components and unrequested query rows are not read or numerically
validated. In particular, a bad unrequested index/t does not fail this request.
There is no global topology scan; coordinates may repeat or reverse.

## Confirmed reconstruction and numerical versions

Widen each demanded Float32 input exactly to Float64. Reconstruct each demanded
absolute handle control component by correctly rounding anchor-plus-offset to
Float64, nearest/ties-to-even. These reconstructed controls define the curve,
consistently with CRV-02. Raw anchor controls preserve their widened bits.

Strict evaluates the mathematical Bezier polynomial of those controls at the
exact supplied t, then correctly rounds once directly to output Float32/Float64.
The two independently named CPU accelerated versions allow at most four final
output-dtype representable steps from strict. At t=0/1, all versions correctly
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
fall back to strict and report actual counts/reasons. Work/capacity exhaustion
fails explicitly. Results for a fixed profile/input/query do not depend on the
request partition, neighboring queries, thread order or cache state.

## Complete interface and polynomial

| Ordered input | Descriptor |
| --- | --- |
| 0 anchors | Float32/Float64 [K,D] |
| 1 handles | Float32/Float64 [K-1,degree-1,D] |
| 2 segment_indices | Int64 [N] |
| 3 t | Float32/Float64 [N] |

All ports are required, generic Values. Output values is a generic Value [N,D]
with empty facets. Static degree is required Int64 2 or 3; static dtype is
required String float32/float64, with constructor default float64. Direct nodes
supply both. No output axis, extrapolation, automatic segment selection, squeeze
or implicit dtype conversion from integers is provided. Shape/dtype inference
uses descriptors and these static parameters, without numeric input execution.

Require 0<=segment_indices[i]<K-1 and finite 0<=t[i]<=1 for requested rows.
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

For requested output Q, the row projection of Q determines Control reads of
segment_indices and t. Validate those controls before reading anchors/handles.
For (i,c) with selected segment j, endpoint paths have anchor Data {(j,c)} or
{(j+1,c)}. Interior paths require anchors {(j,c),(j+1,c)} and handles
{(j,r,c):0<=r<degree-1}. Deduplicate identical source locations across repeated
queries. Both anchors and all local handles remain required in the interior,
even when a value simplification could hide a contribution.

Empty Q reads no runtime payload. No global anchor/handle validation occurs.
Recognized typed Validation closure is separately declared and retained;
generic numeric column isolation cannot bypass an attached typed semantic
obligation. Upstream execution keeps its own transitive support and failures.

Changed segment_indices[i] or t[i] invalidates all dependent components in row i.
Changed anchors[j,c] or handles[j,r,c] invalidates only observations retaining
those exact Data/Validation coordinates. An interior shared-anchor change can
affect either incident segment; unselected component changes do not affect other
components. Controls determine future source selection, so cached plans retain
their row control witnesses as well as source versions. Metadata/static changes
revalidate the plan and output descriptor.

Return owned packed fragments at the actual global rank-2 output Regions with
matching storage origins. Accept arbitrary valid immutable source strides and
offsets, including negative/zero strides and unaligned values. No whole-N*D
allocation, writable source alias or implicit missing zero is permitted. Result
owners survive context teardown and release backing only at final owner release.

## Algorithm, resources and errors

Classify and validate required row controls, gather selected scalar component
controls, reconstruct them with RN64, then evaluate the exact polynomial or a
certified equivalent. An optional reconstructed-control cache is keyed by segment,
component, degree, profile and source witnesses and cannot pre-read other
components. For P requested rows and M requested component values, ordinary
work is O(P+M*degree), plus exact-rounding/certification and typed-validation work.
No work proportional to all K or all D is required for sparse component requests.

Output payload is b*M; active scalar controls require 8*(degree+1) bytes per
admitted lane, plus local inputs, indices and exact-arithmetic limbs. Account
all row/control/dedup metadata, source owners/windows, temporary capacity growth,
cached reconstructed controls, output fragments and validation. Reserve before
allocation and use host workers/work/stage/capacity limits. Poll cancellation
before reads, at least every 64 rows/components, within long arithmetic and before
publication. No unbudgeted private pool or implicit disk backing is introduced.
Cache-off preserves active owners and numerical results.

| Trigger | Phase and Status |
| --- | --- |
| Missing/malformed degree or dtype | Compile/preflight; InvalidArgument / InvalidDomain |
| Wrong dtype/rank/shape relation or size cap | Compile/preflight; TypeMismatch |
| Requested segment outside [0,K-2], or t outside finite [0,1] | Evaluation; InvalidArgument / InvalidDomain |
| Nonfinite demanded anchor/handle component | Evaluation; OperationFailed / InvalidDomain |
| Nonfinite RN64 reconstructed control or actual output conversion overflow | Evaluation; OperationFailed / ArithmeticOverflow |
| Resource, cancellation, stale, unsupported backend, typed or upstream failure | Preserve baseline host Status and identity |

Numeric failures name the affected output Atom (i,c); row-control failures apply
to each dependent requested component. No domain-wide revocation of independent
observations is implied. Preserve offending port/index and available query detail
in bounded diagnostics. Failed observations publish no partial Value; independent
outcomes require the host's eligible atom API, while ordinary execution is fail-fast.

## Acceptance and implementation boundary

Quadratic fixture: anchors=[[0,0],[2,0]], handles=[[[1,2]]],
segment_indices=[0,0,0], t=[0,0.5,1] -> values=[[0,0],[1,1],[2,0]].
Cubic fixture: anchors=[[0,0],[3,0]], handles=[[[1,3],[-1,3]]],
segment_indices=[0], t=[0.5] -> [[1.5,2.25]]. Use independent exact Bernstein
polynomials after the separately required RN64 reconstruction, not the production
de Casteljau helper, as oracle. Strict compares destination bits; accelerated
checks ULP, converted-control bounds, zero classification and fallback reporting.

Test linear/constant degeneracies, loops/reversals, multiple segments, shared
endpoints and corners, repeated/unsorted rows, D=1 without squeezing and large
D with sparse component requests. Include mixed dtypes, RN64 control midpoints,
actual reconstruction overflow, final cancellation, subnormals and signed zeros.
An endpoint request must ignore invalid handles and the opposite endpoint; a
component request ignores unrelated components and unrequested invalid index/t.
Verify these by exact read logs and dirty mappings, including typed closures.

The future public fixture binds all four inputs, supplies degree/dtype, names
values and executes through Compiler/ExecutionContext. Delivery includes actual
build/run commands and output inspection, full/partial comparisons, negative/
zero strides, work/capacity/stage failure, cancellation, cache-off and lifetime
after context destruction. No target registry implementation, runtime conformance
or platform benchmark is claimed by this specification.

- [Curve category](../curves.md).
- [Operator template](../../00-foundation/spec-template.md).
- [Foundation execution contract](../../00-foundation/contracts.md).
