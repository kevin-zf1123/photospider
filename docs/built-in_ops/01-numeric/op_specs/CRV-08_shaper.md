---
spec_schema_version: 1
id: CRV-08
category: 01-numeric
kind: shared_operator_contract
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

# CRV-08: scalar coordinate shapers

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


The initial scope has separately named linear_shaper, log2_shaper and their
inverse forms. These are explicit numerical domain transforms: ordinary
Float32/Float64 input arrays produce same-shape generic numerical arrays for LUT
coordinates. They do not perform tone mapping or silently change a color-array
transfer description.

The linear pair are independent named composite workflow templates reusing
NUM-06B remap_range. Forward maps dynamic [lower,upper] to [0,1], inverse maps
[0,1] back to that interval. Preserve the whole-formula correct rounding of the
underlying primitive. The logarithmic pair need their own nonlinear contracts.

The following numerical and execution clauses complete this initial clarification.

All four interfaces expose dynamic input plus lower[1], upper[1], all sharing
Float32 or Float64 dtype. Bounds are shared across the entire input, finite and
strictly ordered lower<upper; values retains input shape/dtype as generic numeric
data. Linear templates explicitly broadcast scalar boundaries. The inverse
linear template must enforce bound order too, rather than inherit remap_range's
more permissive target-bound behavior.

The log2 pair requires 0<lower<upper. Forward is
(log2(x)-log2(lower))/(log2(upper)-log2(lower)); inverse is
lower*(upper/lower)^t. These are exact mathematical formulas, not sequential
rounded logarithms/division. Do not form an overflowing rounded upper/lower or
silently shift/abs nonpositive x. Special-value behavior is specified below.

All four forms extend beyond their nominal intervals without implicit clipping.
Finite linear inputs and finite inverse coordinates may be outside [lower,upper]
or [0,1]. Positive finite log2 inputs may lie outside [lower,upper]. Explicit
clamp is the composition point for callers requiring range limitation; no
out_of_domain mode is introduced here.

Adopt NUM's IEEE-style numerical values after mandatory bound validation. Input
NaN propagates with payload/sign preserved and signaling NaN quieted. Linear
forms preserve their remap_range infinity/overflow behavior. Log2 forward maps
either signed zero to -Inf, negative finite values and -Inf to canonical quiet
NaN, and +Inf to +Inf. Log2 inverse maps +Inf to +Inf and -Inf to +0. Numerical
overflow/underflow returns the correctly classified floating value, not a
curve-style finite-value failure. Invalid bounds still fail and take precedence
over input NaN. Linear inverse maps signed infinities to like-signed infinities;
the +0 negative-infinity extension belongs only to the log2 inverse.

Linear and log2 forward/inverse strict correctly round their whole formulas.
Accelerated Apple Silicon and x86-64 versions obey the shared final FP32 4 ULP
contract, including its strict fallback range, while matching
strict NaN/Inf/zero classification and signs. Exact source endpoints return
0/1, and inverse t=0/1 returns lower/upper exactly, on every profile. Fall back
to strict and report the fallback when the bound cannot be guaranteed; host
budget exhaustion remains an explicit failure.

Accelerated log2 forward and inverse must also be monotone nondecreasing in
their ordered real input domains for fixed valid bounds. Results are independent
of request partition/order. Prove monotonicity for the combined fast/strict
fallback mapping; per-value 4-ULP checks alone are insufficient. Do not sort a
requested output batch to repair an invalid implementation.

## Shared execution contract

Input rank is 1..8, all extents positive and logical count <=2^40. Ordered ports
are input, lower, upper; output values retains input dtype/shape with empty
facets. There is no output dtype conversion parameter. For requested indices Q,
read only input[Q] but always read/validate both scalar bounds for every nonempty
request, even endpoint and NaN paths. Empty Q reads no dynamic payload. A scalar
bound error affects all observations depending on it; input changes affect only
their own observations. All source descriptors and typed/upstream validation
closures remain explicit and retain provenance.

Static log2 primitive keys select strict/apple_silicon/x86_64 independently;
there is no runtime mode parameter. Linear authoring templates take static
profile=strict/apple_silicon/x86_64, default strict, and expand the corresponding
NUM operation keys. No template computation occurs at construction and no result
is automatically materialized or persisted.

Log forward checks bounds before source special values; NaN quieting and generated
canonical NaN bits follow NUM-04. Forward x=lower gives +0 and x=upper gives 1.
Negative zero source follows log(0) and yields -Inf. Inverse t=+0/-0 gives lower
bits, t=1 gives upper bits; other finite t yields a positive exact result, rounded
with gradual underflow. Linear zero/endpoint/payload semantics are exactly those
of remap_range, not newly redefined by the template.

Return immutable packed regions with correct global origins and owner lifetime
beyond context teardown. Source arbitrary legal offsets, unaligned access and
negative/zero strides are supported. Cache identity includes source/bound witnesses,
profile and template expansion identity. Cache-off and fragmented requests have
the same values, metadata and failure scope as joint requests.

For M requested values, output payload is M*sizeof(dtype); basic work is O(M)
plus certified numerical refinement for log functions. Account source windows,
owners, broadcast views, template intermediates, output capacity, exact arithmetic
and overlapping scratch growth under host capacity/work/stage limits. No allocation
proportional to unrequested logical elements is required. Poll cancellation at
least every 64 simple samples and during each extended arithmetic refinement.
Release temporary state on all terminal paths and publish no partial failed atom.

Invalid bound values/order/positivity fail InvalidArgument/InvalidDomain with
offending port/value and affected output coordinate; bounds take precedence over
input NaN. Compile/preflight checks shape/dtype/static profile, using TypeMismatch
for port incompatibility and InvalidArgument/InvalidDomain for malformed statics.
Numerical domain/overflow outcomes described above are successful IEEE values.
ResourceExhausted, BackendUnavailable, cancellation, stale and upstream failures
retain the NUM shared error categories and provenance. Accelerated fallback is
reported through execution diagnostics without changing the output schema.

## Linear composition and order guard

Forward expands broadcast lower/upper plus constant 0/1 views into remap_range:
source=[lower,upper], target=[0,1]. Inverse uses source=[0,1], target=[lower,upper].
Since remap_range permits unordered target bounds, inverse must guard lower<upper.
A composition using existing primitives computes scalar checked_lower with
remap_range(input=lower,source_lower=lower,source_upper=upper,
target_lower=lower,target_upper=lower), then broadcasts checked_lower into the
inverse target_lower port. This reads both bounds and validates order before a
result can be produced while preserving lower bits. No new assertion primitive
or observable guard output is introduced. Constant views and broadcasts use the
same static profile and input dtype, and internal shapes are known at construction.

## Acceptance and current status

With lower=1, upper=16, log forward x=[1,2,4,16] gives [0,0.25,0.5,1]; inverse
reconstructs [1,2,4,16]. Outside-range x=0.5 and 32 map to -0.25 and 1.25;
inverse agrees at those exact powers. General rounded forward/inverse composition
is not guaranteed to recover the original input bits. Linear lower=-2,upper=2,
x=[-2,0,2,4] gives [0,0.5,1,1.5] and inverse recovers these exact inputs.

Use an independent exact rational oracle for linear and certified whole-expression
logarithm/exponent enclosures for log2, not rounded calls to individual log2/pow.
Exercise near-equal positive bounds, extreme finite bounds, exact endpoint bits,
NaN payloads/signaling, signed zero, infinities, subnormals and output overflow.
Accelerated acceptance verifies per-result 4 ULP, classifications, monotonicity
and cross-fallback/request-partition consistency. Test invalid bound precedence,
partial reads, shape/strides, exact dirty support, cache-off, resource cancellation
and owners after context destruction.

The public workflow and oracle exercise these Proposed interfaces through
Compiler/ExecutionContext; the maintained command and current validation boundary
are linked below.

- [Linear template](CRV-08A_linear_shaper.md).
- [Inverse linear template](CRV-08B_linear_shaper_inverse.md).
- [Log2 primitive](CRV-08C_log2_shaper.md).
- [Inverse log2 primitive](CRV-08D_log2_shaper_inverse.md).

- [Curve category](../curves.md).
- [NUM-06 remap_range](NUM-06B_remap_range.md).

## Maintained implementation and validation

The two linear authoring helpers are `linear_shaper` and `linear_shaper_inverse`
in `photospider/numeric/shapers.hpp`; they expand to existing remap/constant
workflow nodes and are not separate linear primitive registrations. The six log
primitive keys are exposed by `log2_shaper_node` and `log2_shaper_inverse_node`
across strict, Apple and x86 profiles. Log evaluation uses a certified whole
expression with a strict certified scalar fallback and preserves monotonicity and partition
independence. Refinement precision is bounded to 128..4096; unresolved capacity or
rounding returns `ResourceExhausted`.

See [the shaper workflow README](../../../../examples/numeric_workflow/README.md)
and [math implementation](../math-implementation.md) for the target command and
shared fixture. Native Clang 21 strict/Apple and Ubuntu WSL Clang 18 strict/AVX2
passed 4,196 independent Fraction/directed-MPFR cases per profile. All five
manual groups passed all four profiles. Installed 0.16 consumers, the focused
compiler unit, ClangFormat 21/cpplint and independent math/entry reviews passed. This target has no integration-test registration.
