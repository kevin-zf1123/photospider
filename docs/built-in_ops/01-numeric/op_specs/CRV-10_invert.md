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

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM execution baseline](NUM_common_contract.md) for specification/
registration status, CPU target identity, floating environment, diagnostic provenance,
host observation isolation, resource accounting and public/performance acceptance.
In particular, rounding is nearest/ties-to-even with gradual underflow and the
caller floating environment is preserved. This is limited inheritance: the ports,
output structures, observation units, mathematical rounding boundaries and
explicit numerical/error rules in this specification take precedence. It does not
turn a composite template or structured Result into a generic NUM Tensor primitive.
A stated four-ULP final bound uses the shared FP32-scaled contract for both
Float32 and Float64 outputs.
Classification, exact landmarks and signed-zero rules are checked separately.

The initial scope inverts a strictly monotone one-dimensional table: given
knots/values and a query y, return x. Three-dimensional numerical LUT inversion
is outside this initial scope. Separate invert_linear and invert_pchip operations
invert their corresponding forward interpolation functions. Linear solves its
selected segment directly; PCHIP solves the corresponding cubic segment, rather
than swapping x/y and fitting a different PCHIP curve. The following clauses
complete the initial clarification; the runtime implementation is available
through the six registered keys while this specification remains Proposed.

Each dynamic input is a Result containing one tensor member under any schema id and member key; `sample_shape()` gives x[K], y[K] and query[N]. x is finite and strictly increasing;
y is finite and either strictly increasing or strictly decreasing globally.
Plateaus/repeated y values violate the caller contract. Query contains target y values and may repeat or be unordered. Global x/y topology is a caller precondition, stronger than forward interpolation's local finite-y requirement.

Static out_of_domain is reject/clamp, default reject. Compare query against the
actual numeric range of y; clamp selects the corresponding x endpoint, accounting
for y direction. No inverse of extrapolated linear/PCHIP tails is provided.

x/y/query independently accept Float32/Float64 and may mix. Output port `values` is a Result with schema `photospider.tensor`, member `samples`, shape [N], no attached domain description and static Float32/Float64 dtype, default Float64. K is 2..65536; N is
1..2^40. Each selected output tile reads complete x/y and its query row range. Consumed queries and selected final x values must be finite; final representability is a caller precondition.

Strict inverts the forward mathematical curve before destination rounding, then
correctly rounds x directly to output dtype. It does not invert the many-to-one
floating output map or an accelerated approximation. Linear and PCHIP accelerated Apple Silicon/x86-64 allow at most
4 FP32-scaled ULP final x error, while preserving inverse monotonic direction and the selected
x segment's converted bounds. A small forward residual alone is not an x-error
guarantee near zero derivative. Request order/partition cannot affect results.

Exact y-knot hits and clamp directly convert corresponding x knots, preserving
zero sign on every profile. Other mathematical zero results are +0; nonzero
underflow preserves sign. PCHIP accelerated falls back to strict
when error, segment range, zero classification or monotonicity cannot be ensured.
Representability is required for selected final x values. An unreturned segment endpoint narrowing to infinity does not invalidate a finite root.

## Exact formulas and algorithms

Input port order is x,y,query; output is named values. Required static String
parameters are dtype and out_of_domain, with constructor defaults Float64/reject.
The three independent CPU operation keys select profile; no runtime mode or
method parameter is accepted. All arrays and outputs carry ordinary numeric samples without domain labels;
no geometry, color or physical unit is inferred.

Linear locates adjacent y indices bracketing query by the global y direction,
then computes x_j+(query-y_j)*(x_(j+1)-x_j)/(y_(j+1)-y_j) as an exact rational
formula followed by one final rounding. There is no cumulative coordinate update.

PCHIP uses precisely the exact secants and endpoint/interior derivative rules
of [CRV-01B](CRV-01B_interpolate_pchip.md), without rounding derivatives first.
For selected x interval define its exact Hermite cubic p(t), t in [0,1], and
solve p(t)=query. Strictly monotone y knots give a uniquely ordered curve on
each segment, including possible isolated zero derivatives. Convert the exact
root to x=x_j+t*(x_(j+1)-x_j), then correctly round x only once. At K=2 the
curve is the same straight line as invert_linear.

An executable strict strategy brackets the unique root, evaluates the rational
polynomial at candidate dyadic x values exactly, and uses monotonicity to choose
the interval. Once adjacent destination representable numbers bracket the root,
compare against their exact midpoint to decide nearest/ties-to-even. Handle
exact midpoint roots and finite/overflow boundary explicitly. Certified interval
refinement is an alternative; fixed iteration counts or small residual are not
substitutes for correct rounding. Budget every refinement and limb allocation.

Accelerated may use polynomial/Newton/bracketing approximations only with the
final x bound and monotone mapping guarantee. Prove the combined fast/fallback
mapping as a function of query; sorting one output batch does not satisfy this.
The range is between selected x endpoints converted to the destination, and
classification/sign must match strict. No GPU implementation is specified.

## Fixed execution, resources and caller preconditions

Each selected output tile lowers to one fixed CPU Host invocation. It declares complete x and y support and only the corresponding query row interval, closed through each source's own tile grid. Atomic source axes can enlarge the physical read set. Query values may be unordered or repeated. The task searches y directly through borrowed views, using the direction of its first two knots; it passes the original x/y stencil to the exact inverse algorithm.

The entire x and y arrays must be finite, x must strictly increase, and y must be globally strictly increasing or decreasing without plateaus. Consumed query values must be finite and within the closed numeric y range under `reject`. Selected final outputs must be finite and representable. These are unchecked caller preconditions. Preparation checks schemas, static parameters, canonical outputs, exact read membership and scratch; runtime performs no full-array topology or query-validation scan.

Only selected global output coordinates are evaluated and allocated. Each invocation constructs its own `ExactCurve` in preallocated Host scratch of `sizeof(ExactCurve)`, including the 96-slot arena and ratio workspace. No promoted x/y arrays or per-query certificates are retained. Search costs `O(R log K)` for R rows of a task, plus local inverse arithmetic/refinement. Input tile owners, selected output allocations and granted-lane scratch require admission; the full logical output need not fit in memory.

Empty selection emits no payload task while static metadata remains checked. Rebinding reuses immutable preparation and creates independent Run-owned output; there is no content-cache or repeated-demand identity guarantee. Legal immutable signed/zero strides and unaligned offsets are supported through the compiled views. Result owners survive context destruction. Successful task tiles can feed internal dependencies; public call Results appear only after all required tasks succeed and ordered retirement occurs.

Cancellation is polled during binary search, output traversal and exact root/limb operations. Internal arena, product, shift and zero-denominator guards remain active. PCHIP lattice comparison retains its segment-interior bounds, exact infinity-boundary midpoints and finite termination rules. Accelerated candidates require the existing unique-rounding certificate; otherwise exact evaluation proceeds. Fixed invocations do not expose per-value fallback counters.

## Errors and acceptance

Malformed static parameters fail compile/preparation with `InvalidArgument`; incompatible dtype, rank, shape or size returns the corresponding metadata failure (`TypeMismatch` or `InvalidArgument`). Resource exhaustion, unavailable backends, cancellation, stale inputs and required upstream task failures retain their operational categories. Numerical finite-value, topology, reject-domain and output-representability preconditions have no promised rejection diagnostic. A failed call publishes no partial successful Result.

Linear fixture x=[0,1,3],y=[0,2,4],query=[3,1,3] -> [2,0.5,2]. PCHIP fixture
x=[0,1,2],y=[0,1,4],query=[0.3125,2.1875] -> [0.5,1.5]. Negating y and query
keeps these x results. These exact dyadic examples must pass across all versions.
Use independent rational/certified-polynomial references, zero derivative near
endpoints, extreme scales, narrow x intervals, midpoint roots, K=2, both strict y directions, clamp, signed zeros, mixed dtype and finite roots whose unselected endpoints exceed the destination range.
Forward(inverse(query)) need not recover query bits after both outputs round.

Validate PCHIP 4-ULP x bounds, classifications, segment bounds and cross-fallback
monotonicity for all supported profiles, independent of request order/partition.
Inspect full x/y and selected-query tile support. Test strides, concurrent Runs, rebinding, low budgets, cancellation and owner lifetime. A temporary public workflow should bind
x/y/query and static dtype/policy through Compiler/ExecutionContext. Create this check for the current change and remove it after validation.

- [Linear inverse](CRV-10A_invert_linear.md).
- [PCHIP inverse](CRV-10B_invert_pchip.md).

- [Forward interpolation family](CRV-01_interpolate.md).
- [Curve category](../curves.md).

## Maintained implementation and validation

`plugins/ops/01-numeric/curve_inverse.cpp` registers six fixed CPU Result keys through `fixed_curve.hpp`; the public constructors are `invert_linear_node` and `invert_pchip_node` in [`inverse_curves.hpp`](../../../../plugins/ops/include/photospider/ops/numeric/inverse_curves.hpp). Strict and Float64 linear inversion use exact rational evaluation; PCHIP uses the exact polynomial/lattice algorithm. Accelerated Float32 linear and PCHIP may first use certified bracket refinement and accept only a uniquely rounded result. Unresolved cases use their exact rational or polynomial/lattice paths. Exact cross products reduce collinear PCHIP stencils to linear inversion; K=2, knot and clamp paths remain direct. Scalar integer comparison temporarily selects the strict inverse math profile and restores the surrounding numeric profile afterward. The execution floating-environment guard independently preserves the caller environment.

Temporary validation binds immutable source Results and reads selected outputs through the public Result API. Compare the independent Fraction/polynomial oracle in `oracle/ops/numeric/crv10/reference.py`, using valid caller-domain inputs. Cover caller/worker floating environments, mixed dtypes, signed/zero strides, knot/clamp signed zero, sparse and empty selections, preparation reuse, current bindings, cancellation, static signature rejection and source-owner retirement. At large logical N, bind only the selected query tiles and verify admission against selected output plus fixed scratch, separately from insufficient-budget rejection. Retain the independent oracle and delete temporary runtime checkers after validation.
