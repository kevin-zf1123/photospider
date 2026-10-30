---
spec_schema_version: 1
id: CRV-10
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-10: one-dimensional inverse lookup

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Inherit the [NUM execution baseline](NUM_common_contract.md) for specification/registration status, CPU target identity and floating environment. Rounding is nearest/ties-to-even with gradual underflow. This is limited inheritance: the ports, output structures, observation units, mathematical rounding boundaries and explicit numerical/error rules in this specification take precedence. A stated four-ULP final bound uses the shared FP32-scaled contract for both Float32 and Float64 outputs. Classification, exact landmarks and signed-zero rules are checked separately.

The scope is inversion of a strictly monotone one-dimensional table: given knots/values and a query y, return x. Three-dimensional numerical LUT inversion is outside this scope. Separate invert_linear and invert_pchip operations invert their corresponding forward interpolation functions. Linear solves its selected segment directly; PCHIP solves the corresponding cubic segment, rather than swapping x/y and fitting a different PCHIP curve.

## Interface

Each dynamic input is a Result containing one tensor member under any schema id and member key; `sample_shape()` gives x[K], y[K] and query[N]. x/y/query independently accept Float32/Float64 and may mix. K is 2..65536; N is 1..2^40. Input port order is x,y,query; output is named `values`, a Result with schema `photospider.tensor`, member `samples`, shape [N], no attached domain description and static Float32/Float64 dtype. Required static String parameters are `dtype` and `out_of_domain` (reject/clamp), with constructor defaults Float64/reject. The public constructors are `invert_linear_node` and `invert_pchip_node`; each of the two operations has strict, apple_silicon and x86_64 keys (six in total). No runtime mode or method parameter is accepted. All arrays and outputs carry ordinary numeric samples without domain labels; no geometry, color or physical unit is inferred.

Malformed static parameters fail with `InvalidArgument`; incompatible dtype, rank, shape or size returns the corresponding metadata failure (`TypeMismatch` or `InvalidArgument`).

## Mathematics

x is finite and strictly increasing; y is finite and either strictly increasing or strictly decreasing globally. Plateaus/repeated y values violate the caller contract. Query contains target y values and may repeat or be unordered. Global x/y topology is a caller precondition, stronger than forward interpolation's local finite-y requirement.

With `out_of_domain` reject, the query must lie within the actual numeric range of y; clamp selects the corresponding x endpoint, accounting for y direction. No inverse of extrapolated linear/PCHIP tails is provided.

Strict inverts the forward mathematical curve before destination rounding, then correctly rounds x directly to output dtype. It does not invert the many-to-one floating output map or an accelerated approximation. Linear and PCHIP accelerated Apple Silicon/x86-64 allow at most 4 FP32-scaled ULP final x error, while preserving inverse monotonic direction and the selected x segment's converted bounds. A small forward residual alone is not an x-error guarantee near zero derivative. Request order/partition cannot affect results.

Exact y-knot hits and clamp directly convert corresponding x knots, preserving zero sign on every profile. Other mathematical zero results are +0; nonzero underflow preserves sign. Representability is required for selected final x values. An unreturned segment endpoint narrowing to infinity does not invalidate a finite root.

Linear locates adjacent y indices bracketing query by the global y direction, then computes x_j+(query-y_j)*(x_(j+1)-x_j)/(y_(j+1)-y_j) as an exact rational formula followed by one final rounding. There is no cumulative coordinate update.

PCHIP uses precisely the exact secants and endpoint/interior derivative rules of [CRV-01B](CRV-01B_interpolate_pchip.md), without rounding derivatives first. For selected x interval define its exact Hermite cubic p(t), t in [0,1], and solve p(t)=query. Strictly monotone y knots give a uniquely ordered curve on each segment, including possible isolated zero derivatives. Convert the exact root to x=x_j+t*(x_(j+1)-x_j), then correctly round x only once. At K=2 the curve is the same straight line as invert_linear. Fixed iteration counts or small residual are not substitutes for correct rounding: when adjacent destination representable numbers bracket the root, the exact midpoint decides nearest/ties-to-even, and exact midpoint roots and the finite/overflow boundary are handled explicitly.

Accelerated results satisfy the final x bound and a monotone mapping as a function of query; sorting one output batch does not establish that. The result lies between the selected x endpoints converted to the destination, and classification/sign match strict. No GPU implementation is specified.

## Behavior

Each output element depends on the complete x and y arrays and its own query value. Query values may be unordered or repeated. Region and tile results equal the corresponding slice of the whole result.

The entire x and y arrays must be finite, x must strictly increase, and y must be globally strictly increasing or decreasing without plateaus. Consumed query values must be finite and within the closed numeric y range under `reject`. Selected final outputs must be finite and representable. These are unchecked caller preconditions (no full-array topology or query-validation scan); violations have no promised rejection diagnostic. Operational failures retain their categories.

## Acceptance

Linear fixture x=[0,1,3],y=[0,2,4],query=[3,1,3] -> [2,0.5,2]. PCHIP fixture x=[0,1,2],y=[0,1,4],query=[0.3125,2.1875] -> [0.5,1.5]. Negating y and query keeps these x results. These exact dyadic examples must pass across all profiles.

Expected values come from independent rational/certified-polynomial evaluation. Cover zero derivative near endpoints, extreme scales, narrow x intervals, midpoint roots, K=2, both strict y directions, clamp, signed zeros, mixed dtype and finite roots whose unselected endpoints exceed the destination range. Forward(inverse(query)) need not recover query bits after both outputs round. Validate PCHIP 4-ULP x bounds, classifications, segment bounds and monotonicity for all supported profiles, independent of request order/partition.

## References

- [Linear inverse](CRV-10A_invert_linear.md).
- [PCHIP inverse](CRV-10B_invert_pchip.md).
- [Forward interpolation family](CRV-01_interpolate.md).
- [Curve category](../curves.md).
