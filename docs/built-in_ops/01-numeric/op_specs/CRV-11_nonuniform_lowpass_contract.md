---
spec_schema_version: 1
id: CRV-11-NONUNIFORM
parent_id: CRV-11
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-11: nonuniform low-pass family

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

Use shared positions[K] and values, with static axis naming the values dimension
of length K. Other axes identify independent signals. Return values with the same
shape, evaluated at the original positions. K=2..1048576; positions are finite
strictly increasing. New query positions remain a separate resampling step.

The continuous source is the piecewise-linear function through positions/values.
Convolution integrates coordinate length, not sample count. Five independent
kernels correspond to Hann/Hamming/Blackman/Kaiser windowed sinc and truncated
Gaussian.

Kernel parameters are static and finite. support_radius>0 and Gaussian sigma>0
use the position coordinate unit. Sinc cutoff>0 is cycles per position unit;
Kaiser beta>=0 is dimensionless. No universal cutoff<0.5 bound is imposed because
there is no single sampling step. No hidden normalization by mean/minimum/maximum
sample spacing changes these units.

Boundary is reflect/replicate/zero/wrap, default reflect, over the continuous
domain [positions[0],positions[K-1]]. Reflect mirrors the piecewise-linear signal
at domain endpoints; replicate extends endpoint values; zero is zero outside;
wrap uses the domain length as period. Wrap permits different first/last values
and the resulting seam jump; no extra connecting segment is invented. Endpoint
values at an isolated seam have zero measure in the convolution integral.
The mathematical result at each source position x is
integral_{-R}^{R} k(s)*extended_f(x-s) ds / integral_{-R}^{R} k(s) ds.
Strict correctly rounds that complete integral quotient at output. Keep the exact
piecewise-linear reconstruction and mathematical kernel; a high-density uniform
resampling followed by discrete filtering is not the strict reference. Boundary
handling retains full-kernel normalization.

positions and values independently accept Float32/Float64 and may mix. Output
retains values dtype. Finite strictly increasing positions, finite contributing source endpoint values and finite representable selected outputs are unchecked caller preconditions. The fixed kernel consumes these numerical facts directly. The continuous real-signal contract and its source endpoint rules govern every positive-length reconstructed segment.

Every selected output tile lowers to one ABI 8 CPU task. It reads the complete positions vector and all positions along the filtered values axis for the selected other-axis coordinates, closed on each source's own tile grid. Dynamic positions determine which intervals contribute numerically, so the static values closure is a conservative superset. Extra physical samples are not numerical operands. Each task directly searches immutable source views and streams positive-length pieces; isolated contacts do not add endpoints, and coefficient cancellation does not omit an endpoint from its piece.

Only selected output tiles are allocated and computed. Each invocation owns fixed arithmetic, traversal and polynomial scratch; it retains two current pieces and borrows positions without copying a coordinate vector. Empty selection checks static metadata and emits no task or payload read. The executor publishes a successful call after all its writes succeed and retire. Escaped Results retain their storage owners.

Strict correctly rounds the complete integral quotient; accelerated Apple Silicon and x86-64 keys allow the shared final FP32 bound and currently evaluate through the strict algorithm. Precision or fixed arithmetic capacity exhaustion returns ResourceExhausted. Cancellation/resource failures propagate before a shortcut or precision retry; numerical uncertainty alone selects a higher order or precision.

When every participating finite source/extension sample has identical bits,
retain that constant including -0. Otherwise exact zero is +0 and nonzero
underflow keeps the mathematical sign. Virtual zero padding contributes +0 and
may prevent the constant-bit shortcut near boundaries.

## Kernel and boundary formulas

For support R, use the window formulas of
[the uniform contract](CRV-11_uniform_lowpass_contract.md) with continuous u=s/R.
Windowed sinc has k(s)=sincpi(2*cutoff*s)*w(s/R), |s|<=R; Gaussian has
k(s)=exp(-s*s/(2*sigma*sigma)), |s|<=R. All are zero outside. Constants are exact
rationals and pi/I0/exp are mathematical values. Normalization uses the exact
integral over the full support, not the discrete tap sum. Positive normalization
and the numeric support/cutoff/sigma/beta domains are unchecked caller premises.
Refine the division and final rounding as needed; budget exhaustion remains an
operational failure, with no separate normalizer-domain validation.

Between p_j,p_(j+1), reconstruction is the exact affine function through v_j and
v_(j+1). Let a=p_0,b=p_(K-1),D=b-a as an exact real difference. Reflect maps a
coordinate by triangular folding of (x-a) modulo 2D into [0,D]. Wrap maps it
by (x-a) modulo D into [0,D); its seam may jump. Replicate chooses the nearest
endpoint outside [a,b]; zero uses +0 there. Arithmetic for x+/-R, periods and
coordinate folds must not overflow merely because an intermediate Float64 value
would be unrepresentable. Underlying finite inputs retain their exact values.

Ordered dynamic ports are `positions` and `values`. Each is a Result containing exactly one tensor member under any structurally valid schema id/version/member key. Use the complete `sample_shape()` including batch axes. `positions` has shape [K], rank 1, Float32/64, finite strictly increasing values and K=2..1048576. `values` has rank 1..8, positive extents and product <=2^40; static axis selects its extent K.

The `samples` output is an immutable `photospider.tensor` v1 Result member with the original full shape and dtype and no attached domain description, evaluated at the original positions. The output retains its backing owners; the schema carries no operation resource configuration. Output uses the full shape as ordinary axes with zero atomic trailing axes. Coverage contains only selected tiles and retains global coordinates.

Static support_radius is a finite positive Float64; boundary is String default
reflect. Four sinc kernels require finite positive Float64 cutoff; Kaiser also
requires finite Float64 beta>=0; Gaussian requires finite Float64 sigma>0.
No irrelevant parameter or output dtype conversion is accepted. CPU profile is
selected by independent operation key, not a mode parameter.

## Algorithm, resources and mapping

Each task declares complete positions and the full filtered axis of its selected values rectangle. Preparation verifies the canonical output, exact ordered input slots, dtype, empty task parameters and scratch size. Source affine views may have negative/zero strides or unaligned addresses. Lowering counts each source closure before enumeration and rejects closures exceeding its record budget. Immutable prepared state is shared; concurrent Runs own separate output and scratch.

The stream partitions support at source knots and boundary folds/seams using exact integer coordinates in units of 2^-1074. Forward traversal chooses the segment immediately right of a cursor; backward traversal chooses the segment immediately left. Wrap preserves its discontinuous seam. Reflect reverses endpoint order on descending branches. Stored segment endpoints define the original affine function; clipped lower/upper endpoints define its positive integration measure.

One traversal establishes the raw-constant shortcut and endpoint amplitude scale. Two radial cursors then test the exact paired-affine identity. Each precision/order attempt replays forward pieces and integrates a certified polynomial against each affine function. The complete kernel normalization remains independent of source boundaries. Source positions are accessed by binary search, without promoted positions or a retained partition vector.

For J pieces across selected output supports, each traversal costs O(J log K) geometry work plus arithmetic and endpoint reads; refinement can replay it. Scratch is independent of K, J and the number of output elements. It contains fixed 12288-bit arithmetic, two piece records and three coefficient arrays with 513, 513 and 1025 interval slots. Two order-512 factors require at most 1025 product coefficients. Blackman's second cosine temporarily uses the product array before convolution. All storage is preallocated and reused by that invocation.

Large support may traverse many periods. Fixed memory does not bound traversal time; cancellation is polled in every piece, binary search and arithmetic loop. Precision/order limits bound each numerical refinement attempt. A failed or cancelled call exposes no partial Result. Empty selection reads no dynamic payload. Rebinding reuses immutable preparation and does not promise content-cache identity.

## Errors and acceptance

Invalid static shape/axis or parameter syntax/type fails preparation with InvalidArgument/InvalidDomain; dtype/shape mismatch uses TypeMismatch. Numeric support_radius/cutoff/sigma/beta domains, position ordering, endpoint finiteness and selected-output representability are unchecked caller preconditions at authoring and execution, so no helper or kernel adds a numerical-domain validation pass. ResourceExhausted, BackendUnavailable, binding/upstream failures and cancellation retain their execution categories. Operational failures never become a numerical retry or success.

Independent oracle uses the exact piecewise-linear reconstruction and certified
continuous convolution. Finite constants under reflect/replicate/wrap are exactly
preserved. If an affine signal's requested support remains within its reconstruction
domain, an even normalized kernel returns its center value exactly. Example:
positions=[0,0.75,2], values=[1,2.5,5], support_radius=0.5, query at the existing
middle position returns 2.5 for every selected kernel. Inserting an additional
collinear knot does not alter the reconstructed continuous function or this result;
an equal-weight irregular sample average would fail that test.

A nontrivial boundary fixture is positions=[0,1], values=[2,2], support_radius=0.5:
zero boundary has strict result [1,1] for every defined even kernel; accelerated
uses the four-step bound. Reflect/replicate/wrap preserve [2,2] exactly in all
profiles. Exactly half the kernel integral sees the constant source at
each zero-padded endpoint. Replacing values with the smallest positive subnormal
of its dtype gives an exact half-subnormal endpoint value: strict ties-to-even
returns +0, and accelerated must match that zero classification. With source
values three times the smallest positive subnormal, the exact endpoint is
1.5 times that minimum; strict returns twice the minimum by ties-to-even, and
accelerated must remain nonzero within its four-step bound. The latter case
distinguishes gradual underflow from flushing every subnormal result to zero.
Together these check the continuous measure and shared floating contract.

Test impulses as piecewise-linear hats, strongly nonuniform gaps, kernel support
touches, exact affine cancellation, wrap seam jumps, multiple reflection periods,
zero boundaries, both dtypes, mixed position precision, subnormal/extreme parameters,
valid finite endpoint extremes, local negative-zero support and representable output boundaries. Check accelerated
4-ULP/classification/constant obligations against the same continuous oracle.

Frequency response applies to the specified continuous kernel and reconstruction;
there is no single input Nyquist implied by nonuniform positions. This low-pass
cannot recover unknown intersample content or undo aliasing already present in
the source. A downsampling workflow must independently measure its reconstruction/
filter/resampling error and attenuation requirements. Uniform and nonuniform
operators need not agree even at equally spaced positions: one filters discrete
samples and the other filters their continuous linear reconstruction.

Test partial delivery, exact mathematical segment selection, independent signal arithmetic, source-grid closures, affine views, low budgets, cancellation in every traversal/refinement phase, concurrent Runs, sparse large logical outputs and escaped owners.
Temporary public acceptance binds positions/values and kernel parameters, then inspects samples through Compiler/ExecutionContext and compares them with the independent oracle.

- [Resampling family](CRV-11_resample_signal.md).
- [Uniform counterpart](CRV-11_uniform_lowpass_contract.md).

## Maintained implementation and validation

This shared contract covers five nonuniform kernels and 15 profile keys. `lowpass_nonuniform.cpp` emits `nonuniform-lowpass-unchecked-v2` CPU tasks. `CheckedLowpassGeometry::piece_at` reads positions through a raw-word accessor. `NonuniformLowpassMath` streams exact pieces and paired-affine identities, while `CheckedLowpassPolynomialBuilder` uses fixed coefficient views with rigorous Taylor remainder bounds. All accelerated keys currently use strict arithmetic; precision 128..4096 and order <=512 may return `ResourceExhausted`.

The independent Fraction/MPFR oracle enumerates boundary copies and preserves endpoint raw tags through a separate reconstruction for local constant-bit detection. Temporary public and direct checks cover numerical outputs, exact static closures, runtime ownership, cancellation, floating environment, fixed scratch bounds and allocation-free invocation. See [CRV-11 umbrella](CRV-11_resample_signal.md).
