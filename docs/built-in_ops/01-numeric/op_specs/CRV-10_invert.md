---
spec_schema_version: 1
id: CRV-10
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-10: one-dimensional inverse lookup

Inherit the [NUM execution baseline](NUM_common_contract.md) for specification/
registration status, CPU target identity, floating environment, diagnostic provenance,
host observation isolation, resource accounting and public/performance acceptance.
In particular, rounding is nearest/ties-to-even with gradual underflow and the
caller floating environment is preserved. This is limited inheritance: the ports,
output kinds/facets, observation units, mathematical rounding boundaries and
explicit numerical/error rules in this specification take precedence. It does not
turn a composite template or structured Result into a generic NUM Value primitive.
Where a 4-ULP final bound is stated, it means at most four adjacent
representable steps in the output dtype from the correctly rounded strict result,
measured by monotone IEEE bit-pattern distance for nonzero finite values.
Classification, exact landmarks and signed-zero rules are checked separately.


The initial scope inverts a strictly monotone one-dimensional table: given
knots/values and a query y, return x. Three-dimensional numerical LUT inversion
is outside this initial scope. Separate invert_linear and invert_pchip operations
invert their corresponding forward interpolation functions. Linear solves its
selected segment directly; PCHIP solves the corresponding cubic segment, rather
than swapping x/y and fitting a different PCHIP curve. The following clauses
complete the initial clarification; runtime implementation remains Proposed.

Dynamic inputs are x[K], y[K], query[N]. x is finite and strictly increasing;
y is finite and either strictly increasing or strictly decreasing globally.
Reject plateaus/repeated y values. query contains target y values, may repeat or
be unordered. Every nonempty request validates all x/y for the required topology;
this global validation is stronger than forward interpolation's local y reads.

Static out_of_domain is reject/clamp, default reject. Compare query against the
actual numeric range of y; clamp selects the corresponding x endpoint, accounting
for y direction. No inverse of extrapolated linear/PCHIP tails is provided.

x/y/query independently accept Float32/Float64 and may mix. Output values[N]
uses static Float32/Float64 dtype, default Float64. K is 2..65536; N is
1..2^40. Requested query values and returned x values must be finite; unrequested
query values are not read/validated. Narrowing overflow fails only dependent
observations, not unrequested outputs.

Strict inverts the forward mathematical curve before destination rounding, then
correctly rounds x directly to output dtype. It does not invert the many-to-one
floating output map or an accelerated approximation. Linear all three versions
are bitwise identical. PCHIP accelerated Apple Silicon/x86-64 allow at most
4 ULP final x error, while preserving inverse monotonic direction and the selected
x segment's converted bounds. A small forward residual alone is not an x-error
guarantee near zero derivative. Request order/partition cannot affect results.

Exact y-knot hits and clamp directly convert corresponding x knots, preserving
zero sign on every profile. Other mathematical zero results are +0; nonzero
underflow preserves sign. PCHIP accelerated falls back to strict and reports it
when error, segment range, zero classification or monotonicity cannot be ensured.
Only demanded final x is checked for representability; an unreturned segment
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

Empty Q reads no payload. Nonempty Q reads and validates all x/y, then only
query[Q]. Linear arithmetic uses the selected pair; PCHIP coefficients use its
local stencil from CRV-01B, but no local arithmetic shortcut suppresses global
topology validation. Invalid remote x/y affects every dependent observation;
unrequested invalid query does not. Both global arrays are exact validation
dependencies, so any x/y change invalidates all dependent inverse observations.
Query changes invalidate only matching output indices. Preserve any wider
typed/upstream support and origin identity explicitly.

Cache identity includes interpolation method, input descriptions/witnesses,
dtype, policy and backend profile. Return immutable packed fragments at the
requested global index origins; arbitrary legal input strides, offsets, unaligned
access and zero/negative strides are supported. Source/output ownership remains
valid after context destruction. Missing requested data is never zero-filled.

For M requested points, base lookup work O(K+M log K) plus exact arithmetic/root
refinement. Retaining promoted x/y costs at most 16K bytes; local PCHIP coefficients
may be computed on demand or in accounted caches. Output is M*sizeof(dtype).
Account source owners/windows, global validation state, limbs, bracketing state,
result fragments and temporary growth overlap under capacity/work/stage budgets.
No full query-sized output is required for partial Q. Poll cancellation during
global scans, at least every 64 simple queries and during every root refinement.
Resource exhaustion fails explicitly without lowering precision; clean all
temporary state on every terminal path and preserve only owned output state.

## Errors and acceptance

Malformed static parameters/count limits fail compile/preflight with
InvalidArgument/InvalidDomain; dtype/shape mismatch uses TypeMismatch. Nonfinite
x/y, invalid global monotonicity, nonfinite demanded query or reject-domain
failure uses OperationFailed/InvalidDomain. Final narrowing overflow uses
OperationFailed/ArithmeticOverflow. ResourceExhausted, BackendUnavailable,
cancellation, stale and upstream errors retain their shared categories/provenance.
Each requested inverse sample is one observation; no partial failed sample is
published. No failure arises merely because an unrequested output would overflow.

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

The current runtime registers six keys through the public
[`inverse_curves.hpp`](../../../../include/photospider/numeric/inverse_curves.hpp)
helpers `invert_linear_node` and `invert_pchip_node`. Linear inverse uses an
exact rational path and is bitwise identical across profiles. PCHIP uses
the exact polynomial/lattice algorithm in strict; accelerated cubic non-knot
queries use the strict scalar fallback, while exact knot/clamp and K=2 paths do
not require fallback.

See the [inverse-curves workflow](../../../../examples/numeric_workflow/README.md#inverse-curves)
for the shared fixture and validation details. Native Clang 21 Strict/Apple and Ubuntu WSL Clang 18 Strict/AVX2 passed all
four manual groups and 404 independent Fraction cases per profile. The installed
0.16 consumer passed both native profiles. WSL checks numerical correctness only.
