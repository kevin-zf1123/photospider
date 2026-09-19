---
spec_schema_version: 1
id: CRV-01B
parent_id: CRV-01
function: interpolate_pchip
operation_family: curve.interpolate_pchip
proposed_operation_keys:
  - curve.interpolate_pchip_strict
  - curve.interpolate_pchip_accelerated_apple_silicon
  - curve.interpolate_pchip_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-01B: interpolate_pchip

Inherit the [NUM baseline](NUM_common_contract.md) for status, registration,
platforms, floating environment, resources, errors and public acceptance, with
the explicit curve rules below taking precedence. Clarification is complete;
the specification remains Proposed independently of the maintained implementation.

## Confirmed interface and scope

Evaluate a single piecewise cubic Hermite interpolating polynomial (PCHIP),
computing node derivatives from the known points rather than explicit handles.
It has an independent operation name from linear interpolation. The separate
multiple-function variant is outside this primitive.

Inherit the confirmed [linear interface](CRV-01A_interpolate_linear.md): ordered
dynamic Value inputs x[K], y[K], query[N] with 2<=K<=65536 and 1<=N<=2^40;
each independently Float32/Float64; output values[N] selects Float32/Float64,
default Float64. Knot x values are finite and strictly increasing. Query order
and repetition are unrestricted; preserve query order in output.

For every nonempty output request, read and validate the complete x array and
read only requested query entries. The PCHIP-specific y support and numerical
profile below replace linear's two-endpoint support and accelerated bit equality.
Output facets are empty, no axis output is added and no units are inferred.
All inputs support the same legal immutable stride/offset layouts as linear.
Required static String parameters dtype and out_of_domain are supplied explicitly
in direct nodes; constructors write float64 and reject respectively. No method,
derivative-order, tension or supplied-tangent parameter is provided.

## Confirmed domain policies

Static out_of_domain is reject, clamp or linear_extrapolate, default reject.
Reject fails an out-of-domain observation. Clamp returns the selected endpoint y.
Linear extrapolation uses the corresponding PCHIP endpoint derivative m:

    left:  y[0]   + (query-x[0])*m[0]
    right: y[K-1] + (query-x[K-1])*m[K-1]

This follows the endpoint tangent, not the endpoint secant or continued cubic
polynomial. The mathematical extrapolation joins with the same first derivative
as the interior curve. Interior no-overshoot properties do not impose endpoint
range clipping on the extrapolated ray.

## Confirmed numerical versions

Strict defines PCHIP slopes and the selected cubic/tangent formula using exact
mathematical arithmetic on the supplied input floats, with only one final
nearest/ties-to-even rounding directly to output dtype. A curve formed from
previously rounded slopes is not the strict mathematical reference.

The independently named Apple Silicon and x86-64 CPU accelerated versions allow
at most four output-dtype representable steps from the correctly rounded strict
result. This final allowance includes slope, polynomial and extrapolation error.
They must also retain the specified interval shape constraints and correctly
rounded exact node hits. The numerical shape and fallback obligations are
defined below, independent of any particular library implementation.

## Confirmed local y support

Exact knot hits and clamp read only the selected knot y. For an interior query
in segment j, read y[max(0,j-1):min(K,j+3)], sufficient to construct its two
endpoint derivatives. At the left or right exterior tangent, read the first or
last min(K,3) y values respectively. K=2 reduces to a straight line.

Do not read or numerically validate other y entries. Global x validation and
requested query support remain unchanged. This fixed local support is retained
even when a slope branch returns zero; input-value shortcuts do not remove its
declared contributing y dependencies. Precise numeric validation is separate.

## Confirmed finite-value and zero rules

All actually read y/query values and the final output must be finite. Negative
and greater-than-one finite values are allowed. Nonfinite demanded input and
output overflow fail the affected observation, while unselected y/query errors
are not evaluated. Exact node hits and clamp correctly convert that y to output
dtype and retain its zero sign in every profile.

Otherwise, if the exact curve result is zero, return -0 only when both endpoint
y values of the selected segment are -0; return +0 for other exact zeros. The
first/last segment determines this rule for its endpoint tangent extrapolation.
Nonzero results rounded to zero retain their mathematical sign. The accelerated
allowance does not override these specified zero cases.

## Exact mathematical definition

All quantities in this section denote exact real/rational arithmetic. Define
h[j]=x[j+1]-x[j] and d[j]=(y[j+1]-y[j])/h[j]. For K=2, both node derivatives
equal d[0]. For an interior node i, set m[i]=0 if either adjacent secant is
zero or their signs differ. Otherwise use

    w1 = 2*h[i] + h[i-1]
    w2 = h[i] + 2*h[i-1]
    m[i] = (w1+w2) / (w1/d[i-1] + w2/d[i])

For K>=3 define an endpoint function E(h0,h1,d0,d1):

    e = ((2*h0+h1)*d0 - h0*d1)/(h0+h1)
    if sign(e) != sign(d0): return 0
    if sign(d0) != sign(d1) and abs(e) > 3*abs(d0): return 3*d0
    return e

Here sign(0)=0. Then m[0]=E(h[0],h[1],d[0],d[1]) and
m[K-1]=E(h[K-2],h[K-3],d[K-2],d[K-3]). For segment j, set
t=(query-x[j])/h[j] and evaluate

    H(t) = (2*t^3-3*t^2+1)*y[j] + (t^3-2*t^2+t)*h[j]*m[j]
         + (-2*t^3+3*t^2)*y[j+1] + (t^3-t^2)*h[j]*m[j+1]

Exact knot/clamp paths select a y before this formula. Exterior queries use
the separately specified tangent formula. Round the chosen complete expression
only at its destination; exact slopes may exceed Float64 without invalidating
a representable final value. A slope branch must be decided from exact secant
signs and exact comparisons, not an underflowed or overflowed approximation.
K=2 has the same exact mathematical function as linear, with this operator's
separately allowed accelerated tolerance.

The interior harmonic mean and one-sided endpoint limiter follow the algorithm
in [SciPy v1.18.0 PCHIP source](https://github.com/scipy/scipy/blob/v1.18.0/scipy/interpolate/_cubic.py).
The target exact arithmetic, query demand and exterior tangent policy remain
Photospider contracts, not numerical compatibility claims about SciPy.

## Accelerated shape, classification and fallback

For fixed input snapshots and a fixed accelerated profile, the result at a query
must be independent of request order, batching, other queries, cache state and
thread schedule. Within each segment, any two finite successful query results
must respect its endpoint y direction: nondecreasing for increasing y,
nonincreasing for decreasing y, and constant for equal y. The result is also
bounded by the minimum/maximum of its two correctly converted output-dtype
endpoints. These comparisons are numerical; explicit signed-zero rules prevail.

An endpoint conversion that would overflow is used only as an extended bound,
not as a reason to reject another finite requested value. Only actual output
conversion/classification decides numerical overflow. At identical source/query
values, strict and accelerated agree on success versus numerical failure and on
zero classification/sign. Nonzero finite accelerated outputs must remain within
four representable steps of the correctly rounded strict result, including
subnormal neighborhoods. Exact knot and clamp conversions match strict bits.

If a fast path cannot guarantee quality, shape and special-value rules together,
fall back to strict and report actual fallback counts/reasons. The combined
fast/fallback function must itself meet the cross-query monotonicity contract;
checking a single approximate value against endpoints is insufficient. Do not
sort or repair a requested batch to create apparent monotonicity. Budget
exhaustion returns ResourceExhausted; it does not authorize an unverified value.
Exterior tangent extrapolation has no interior endpoint-range bound.

## Demand, invalidation and ownership

Use x[0:K] as global Control/Validation and requested query indices Q as Control.
Reject invalid x before segment lookup, and invalid query before dependent y
reads. The y Data set is exactly the union from the local support section. For
K=3 the first/last segment each needs three y values; for K=2 only two. Read the
full fixed local stencil even when a derivative becomes zero, and never infer
additional y demand from an optional full-slope precomputation. A domain reject
reads no y. Empty Q reads no payload.

Retain recognized typed Validation closure and all source/control versions.
Changed x invalidates all retained observations; changed query[i] invalidates
values[i]; changed y[k] invalidates only observations whose local stencil or
selected knot contains k, plus typed-validation effects. Exact node/clamp paths
do not acquire neighbor-y dependencies. Shared slope/index caches retain their
precise x/y/query witnesses and consume host resources. Runtime upstream Whole
support is separate from the declared local support and retains its own failures.

Output is immutable packed owned fragments at requested global indices with
correct Region/storage origins. No whole-N allocation, missing zero or writable
alias is introduced. Output inference is static; owners survive context teardown
until final release. Reuse or fallback cannot erase read witnesses.

## Algorithms, resources and errors

The reference scans all x, constructs an optional budgeted x index, searches each
query, gathers its exact y stencil and evaluates exact slopes/formula. A slope
cache is optional; it cannot read unrelated y to fill itself. Search work is
O(K+M log K) for M requested outputs, plus slope/evaluation arithmetic and typed
validation. At most min(K,4M) distinct y indices are required. Packed x capacity
is at most 8K bytes; requested output payload is b*M. Also account source owners,
active exact slopes, secants, limb growth, segment/dedup metadata and certificates.
No bounded iteration count or fixed libm precision substitutes for correct rounding.

Use host workers/admission and cancellation before reads, at least every 64
knots/queries, within long arithmetic and before publication. Preserve the
floating environment. If available precision/work/capacity cannot resolve the
required result, fail explicitly rather than clipping an unproved approximation.
Cache-off preserves active ownership. No unbudgeted disk, thread pool or full
source/slope/output materialization is implied.

Use CRV-01A's explicit error table and Atom/publication rules: nonfinite demanded
data, invalid x topology or reject-domain query is OperationFailed/InvalidDomain;
actual destination overflow is OperationFailed/ArithmeticOverflow. Correctly
defined slopes do not add a DivideByZero error: branch zero secants before the
harmonic expression. Backend/resource/cancellation/typed/upstream statuses retain
their identities. No failed observation publishes a partial successful Value.

## Acceptance and verification scope

Conceptual public fixture: x=[0,1,2], y=[0,1,4], query=[0,0.5,1,1.5,2]
has exact node slopes [0,1.5,4] and values=[0,0.3125,1,2.1875,4]. With
linear_extrapolate, query=[-1,3] yields [0,8]; this distinguishes tangent
extrapolation from continuing the cubic or using the endpoint secant. Bind all
three inputs, supply dtype/domain policy and request named values through
Compiler/ExecutionContext when implemented; delivery provides actual run commands.

Use an independent exact rational implementation of the formulas and direct
destination rounding. Include irregular x spacing, increasing/decreasing y,
turns, plateaus, exact knots, endpoint-limiter sign/cap branches, K=2, both
output dtypes and mixed source dtypes. Test huge/small secants with finite final
results, cancellation, signed zeros, subnormal boundaries and actual overflow.
Verify endpoint conversion overflow does not reject a finite interior result.

Strict compares bits; accelerated checks final ULP, exact node/zero outcomes and
monotonicity/no-overshoot jointly. Evaluate adjacent representable queries around
knots and extrema, both independently and in changed request partitions. Exercise
the actual fast/strict boundary and fallback diagnostics, rather than assuming
a sampled numerical test alone proves all-input shape preservation.

Read logs must show global x, requested query and the exact local y set, including
single-y node/clamp paths and ignored remote nonfinite y. Test source invalidation,
strides, low budgets, cancellation, cache-off and owner lifetime. These are target
acceptance requirements, not executed product/platform performance claims.

## Source and implementation distinction

The [SciPy PchipInterpolator documentation](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html)
describes a first-derivative-continuous shape-preserving interpolant and a weighted
harmonic-mean rule for interior derivatives. It is an algorithm reference, not
a claim of bitwise compatibility or adoption of its extrapolation defaults.

Current curve.sample_monotone instead uses controls[K,2], a static uniform
domain/count and Whole evaluation. Its rounded-slope computation and y clipping
are recorded in the [family implementation comparison](CRV-01_interpolate.md).
This explicit-query target remains Proposed; its maintained implementation is recorded below.

## Maintained implementation and validation

`plugins/ops/01-numeric/curve_interpolation.cpp` implements these three profile
keys. The public `photospider/numeric/curves.hpp` constructor is
`interpolate_pchip_node`. All profiles currently use exact rational evaluation and one
final destination rounding. Global x validation, requested query rows and the
local y stencil follow the demand contract above.

The [family implementation record](CRV-01_interpolate.md#maintained-implementation-and-validation)
contains the shared arithmetic/resource details and actual platform acceptance.
See [the editable workflow](../../../../examples/numeric_workflow/README.md#explicit-query-curves-crv-01)
for construction, explicit work budgets, commands and checked expected results.
The manual target is excluded from default builds and CTest/integration testing.
Specification status remains Proposed.
