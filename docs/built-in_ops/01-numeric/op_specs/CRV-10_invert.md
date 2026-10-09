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
output kinds/facets, observation units, mathematical rounding boundaries and
explicit numerical/error rules in this specification take precedence. It does not
turn a composite template or structured Result into a generic NUM Value primitive.
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
Reject plateaus/repeated y values. query contains target y values, may repeat or
be unordered. Every nonempty request validates all x/y for the required topology;
this global validation is stronger than forward interpolation's local y reads.

Static out_of_domain is reject/clamp, default reject. Compare query against the
actual numeric range of y; clamp selects the corresponding x endpoint, accounting
for y direction. No inverse of extrapolated linear/PCHIP tails is provided.

x/y/query independently accept Float32/Float64 and may mix. Output port `values` is a Result with schema `photospider.tensor`, member `samples`, shape [N], empty facets and static Float32/Float64 dtype, default Float64. K is 2..65536; N is
1..2^40. Every nonempty request reads all three inputs and computes the complete output. Every query and final x must be finite; an invalid query or output
overflow anywhere fails the run, including outside the delivered footprint.

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
Only final x is checked for representability; an unreturned segment
endpoint narrowing to infinity does not independently reject a finite root.

## Exact formulas and algorithms

Input port order is x,y,query; output is named values. Required static String
parameters are dtype and out_of_domain, with constructor defaults Float64/reject.
The three independent CPU operation keys select profile; no runtime mode or
method parameter is accepted. All arrays and outputs use generic numeric facets;
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

## Demand, dirty propagation and storage

Empty Q reads no payload. All six formal profile keys use Whole. Nonempty Q collects complete x/y/query Results with Data, Validation and Descriptor (role 13), validates global x/y topology, then prechecks every query control before inverse arithmetic. The unchanged
linear pair or PCHIP stencil is selected from complete inputs. Any input change
invalidates the complete output. Full typed/upstream validation can fail even
outside delivered Q. Dynamic numerical failures have Domain/Run scope.

Cache identity includes method, dtype, policy, profile and complete inputs.
The program publishes one immutable dense Result with full certified coverage; sample coordinates remain global rather than being rebased to Q. Its association records the actual source ObjectIds. Arbitrary legal strides, offsets, unaligned and zero/negative
strides are supported. Output ownership survives context destruction.

Lookup work is O(K+N log K) plus exact arithmetic/refinement. Promoted x/y use
16*K element bytes plus allocator/metadata overhead. Fixed arithmetic workspace
and per-root state replace per-output dependency records; complete output uses
N*sizeof(dtype), and collected input storage is also budgeted. Sparse demand can
therefore exhaust capacity that regional execution previously accepted. Work and
cancellation checks cover topology/query scans, search, each root refinement and
publication. Failure releases temporary storage and publishes no partial output.
DependencySession numeric/fallback counters are unavailable for Whole callbacks;
N/A is not a claim of zero fallback. Scalar/NEON/AVX2 math paths remain available.

## Errors and acceptance

Malformed static parameters/count limits fail compile/preflight with
InvalidArgument/InvalidDomain; dtype/shape mismatch uses TypeMismatch. Nonfinite
x/y, invalid global monotonicity, nonfinite query or reject-domain
failure uses OperationFailed/InvalidDomain. Final narrowing overflow uses
OperationFailed/ArithmeticOverflow. ResourceExhausted, BackendUnavailable,
cancellation, stale and upstream errors retain their shared categories/provenance.
Whole output publication is atomic. A final output overflow anywhere fails the run;
an unused endpoint that would narrow to infinity does not reject a finite root.

Linear fixture x=[0,1,3],y=[0,2,4],query=[3,1,3] -> [2,0.5,2]. PCHIP fixture
x=[0,1,2],y=[0,1,4],query=[0.3125,2.1875] -> [0.5,1.5]. Negating y and query
keeps these x results. These exact dyadic examples must pass across all versions.
Use independent rational/certified-polynomial references, zero derivative near
endpoints, extreme scales, narrow x intervals, midpoint roots, K=2, y plateaus,
both y directions, clamp, signed zeros, mixed dtype and output overflow tests.
Forward(inverse(query)) need not recover query bits after both outputs round.

Validate PCHIP 4-ULP x bounds, classifications, segment bounds and cross-fallback
monotonicity for all supported profiles, independent of request order/partition.
Test global versus local failure support, strides, exact dirty witnesses, cache-off,
low budgets, cancellation and owner lifetime. The maintained public workflow binds
x/y/query and static dtype/policy through Compiler/ExecutionContext. See the
inverse-curves example linked below for the command and current validation evidence.

- [Linear inverse](CRV-10A_invert_linear.md).
- [PCHIP inverse](CRV-10B_invert_pchip.md).

- [Forward interpolation family](CRV-01_interpolate.md).
- [Curve category](../curves.md).

## Maintained implementation and validation

 The source/contract reviewer and worker verification found no blocker or required change.

`plugins/ops/01-numeric/curve_inverse.cpp` registers the six Whole Result keys; the public constructors are `invert_linear_node` and `invert_pchip_node` in [`inverse_curves.hpp`](../../../../plugins/ops/include/photospider/ops/numeric/inverse_curves.hpp). Linear inversion uses an exact rational path across profiles. Strict and general Float64 PCHIP use the exact polynomial/lattice algorithm. Accelerated Float32 PCHIP uses a certified bracket and accepts only a uniquely rounded result, otherwise it falls back to the strict scalar solver. Exact cross products reduce collinear PCHIP stencils to linear inversion; K=2, knot and clamp paths remain direct. Scalar integer comparison temporarily selects the strict inverse math profile and restores the surrounding numeric profile afterward. The execution floating-environment guard independently preserves the caller environment.

The maintained manual fixture is `examples/numeric_workflow/inverse.cpp`; it keeps source arrays as Value backing, binds their immutable Results through the public workflow, returns Results and reads them through authorized windows. These are separate from `test_numeric_result_math_curves`'s `inverse_workflows` and `inverse_boundaries` integration cases. The fixture covers both caller and actual computation-worker floating environments, mixed dtypes, negative/zero strides, dirty scope, knot/clamp signed zero, static preparation reuse, repeated-demand ObjectId retention, a true fresh-source cache hit with current direct associations, Empty propagation without polling a failed producer, `SampledSignal` type failure attributed to input 2, cancellation, and source-owner retirement. Retained Float32 and Float64 outputs/read windows account for 12 and 24 Payload bytes; releasing the final window returns Root live capacity to zero.  A public N=2^40 constant View is rejected as `ResourceExhausted/CapacityLimit` at node 1 because Whole execution must allocate the complete output; this is not a numerical run at that physical output size. The installed-consumer target is configured to compile and link the public workflow source against package 0.32.0.
