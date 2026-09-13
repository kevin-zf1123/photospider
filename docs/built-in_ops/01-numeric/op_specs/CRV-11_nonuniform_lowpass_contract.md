---
spec_schema_version: 1
id: CRV-11-NONUNIFORM
parent_id: CRV-11
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-11: nonuniform low-pass family

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
retains values dtype. All demanded source values and final outputs must be finite;
nonfinite samples or final overflow fail the corresponding output observation.
This is a continuous real-signal contract, unlike the uniform discrete family's
explicit IEEE NaN/Inf aggregation. Dependency and accelerated details follow.

Every nonempty output request validates positions globally. Value dependencies
are only endpoints of reconstructed segments intersecting the positive-length
integration support after boundary mapping, for the requested other-axis
coordinates. Replicate outside support reads its endpoint; zero outside reads
no value. Reflections/wraps union their source segments. Isolated support contact
has zero integration measure and does not introduce an otherwise unused segment.
Do not discard actual reconstruction endpoints because integrated coefficients
cancel. Unrelated channels/batches and distant values remain unread.

Strict correctly rounds the complete integral quotient; accelerated Apple Silicon
and x86-64 keys allow final nonzero finite error <=4 ULP and must match strict
finite/zero/sign classification. Report strict fallback when needed. Certified
integration/rounding that cannot finish within available capacity/work/stage
returns ResourceExhausted. Do not publish an unconverged approximation.
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
integral over the full support, not the discrete tap sum. Admit only nonzero
positive normalization; prove it for valid parameters or return the precise
normalizer/domain error, never confuse budget exhaustion with a mathematical zero.

Between p_j,p_(j+1), reconstruction is the exact affine function through v_j and
v_(j+1). Let a=p_0,b=p_(K-1),D=b-a as an exact real difference. Reflect maps a
coordinate by triangular folding of (x-a) modulo 2D into [0,D]. Wrap maps it
by (x-a) modulo D into [0,D); its seam may jump. Replicate chooses the nearest
endpoint outside [a,b]; zero uses +0 there. Arithmetic for x+/-R, periods and
coordinate folds must not overflow merely because an intermediate Float64 value
would be unrepresentable. Underlying finite inputs retain their exact values.

Ordered dynamic ports are positions, values; named output is samples, preserving
values shape/dtype with generic facets. values rank is 1..8, positive extents and
logical count <=2^40. Static axis satisfies 0<=axis<rank and selects extent K.
Static support_radius is a finite positive Float64; boundary is String default
reflect. Four sinc kernels require finite positive Float64 cutoff; Kaiser also
requires finite Float64 beta>=0; Gaussian requires finite Float64 sigma>0.
No irrelevant parameter or output dtype conversion is accepted. CPU profile is
selected by independent operation key, not a mode parameter.

## Algorithm, resources and mapping

Globally validate/promote positions and locate each requested center's support.
Partition the integration domain at reconstructed segment boundaries and boundary
extension folds/seams. On each piece, the source is affine. Analytic antiderivatives
where available or certified quadrature/error enclosures may evaluate kernel
moments against that affine function. Refine until final output rounding or the
accelerated final bound is certified. Fixed quadrature order and an unverified
residual are not sufficient. Do not first create an approximate uniform source.

Large support may traverse many periods or all source intervals. Account all
such work; exploiting repeated periods is allowed only with the same mathematical
and dependency result. For M requested values and J intersected segment pieces,
base work is O(K+M log K+J), plus certified integration. Optional Float64 position
storage is 8K bytes, output payload M*sizeof(values dtype). Charge interval maps,
source owners/windows, normalization enclosures, arithmetic/quadrature state,
coefficient caches and scratch growth overlap under host capacity/work/stage.
No full logical values/output materialization is required for sparse requests.

Position changes invalidate all dependent observations due to global topology and
coordinate validation. Value changes invalidate only observations whose mapped
reconstruction segments use that endpoint, for the same other-axis coordinates.
Retain exact support unions, separate source typed/upstream closures and descriptor
identity. Arbitrary immutable strides, zero/negative strides, offsets and unaligned
source values are supported. Return packed requested samples at global index
origins with owning data/metadata that survive context teardown. Cache-off and
request partitions cannot change values or support.

Poll cancellation during global positions validation, each integration/refinement
piece and at bounded source-read/output batches; release temporary state on all
terminal paths. A compact output does not waive global coordinate work or ancestry
capacity. Empty Q reads no dynamic payload, though static preflight remains.

## Errors and acceptance

Invalid static shape/axis/parameters fail compile/preflight with
InvalidArgument/InvalidDomain; dtype/shape mismatch uses TypeMismatch. Invalid
global positions or nonfinite demanded source samples fail
OperationFailed/InvalidDomain. Final output narrowing overflow fails
OperationFailed/ArithmeticOverflow. ResourceExhausted, BackendUnavailable,
upstream, typed, stale and cancellation errors retain their established identity.
Each requested sample is one observation; unrequested distant values do not fail
it, and no partial failed sample is published.

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
NaN/Inf in demanded versus distant samples and final overflow. Check accelerated
4-ULP/classification/constant obligations against the same continuous oracle.

Frequency response applies to the specified continuous kernel and reconstruction;
there is no single input Nyquist implied by nonuniform positions. This low-pass
cannot recover unknown intersample content or undo aliasing already present in
the source. A downsampling workflow must independently measure its reconstruction/
filter/resampling error and attenuation requirements. Uniform and nonuniform
operators need not agree even at equally spaced positions: one filters discrete
samples and the other filters their continuous linear reconstruction.

Test partial outputs, exact segment/dirty support, other-axis independence,
strides, low budgets, cancellation, cache-off and context-lifetime owners. Future
public workflows bind positions/values, axis/kernel parameters and inspect samples
through Compiler/ExecutionContext with actual commands and independent results.
No current nonlinear-integration runtime or product acceptance is claimed here.

- [Resampling family](CRV-11_resample_signal.md).
- [Uniform counterpart](CRV-11_uniform_lowpass_contract.md).
