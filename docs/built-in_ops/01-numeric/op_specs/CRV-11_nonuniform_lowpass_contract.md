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

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Inherit the [NUM execution baseline](NUM_common_contract.md) for specification/registration status, CPU target identity and floating environment. Rounding is nearest/ties-to-even with gradual underflow. This is limited inheritance: the ports, output structures, observation units, mathematical rounding boundaries and explicit numerical/error rules in this specification take precedence. A stated four-ULP final bound uses the shared FP32-scaled contract for both Float32 and Float64 outputs. Classification, exact landmarks and signed-zero rules are checked separately.

## Interface

Shared positions[K] and values are used, with static axis naming the values dimension of length K. Other axes identify independent signals. The output has the same shape as values, evaluated at the original positions. New query positions remain a separate resampling step. Five independent kernels correspond to Hann/Hamming/Blackman/Kaiser windowed sinc and truncated Gaussian, each with strict, apple_silicon and x86_64 keys (15 profile keys).

Ordered dynamic ports are `positions` and `values`. Each is a Result containing exactly one tensor member under any structurally valid schema id/version/member key. Shapes use the complete `sample_shape()` including batch axes. `positions` has shape [K], rank 1, Float32/64, finite strictly increasing values and K=2..1048576. `values` has rank 1..8, positive extents and product <=2^40; static axis selects its extent K. positions and values independently accept Float32/Float64 and may mix. Output retains values dtype.

The `samples` output is an immutable `photospider.tensor` v1 Result member with the original full shape and dtype and no attached domain description. It uses the full shape as ordinary axes with zero atomic trailing axes.

Static parameters: axis; boundary String (reflect/replicate/zero/wrap, default reflect); support_radius, a finite positive Float64; for the four sinc kernels a finite positive Float64 cutoff; for Kaiser also finite Float64 beta>=0; for Gaussian finite Float64 sigma>0. No irrelevant parameter or output dtype conversion is accepted. CPU profile is selected by operation key, not a mode parameter. support_radius, Gaussian sigma and sinc cutoff use the position coordinate unit (cutoff in cycles per position unit); Kaiser beta is dimensionless. No universal cutoff<0.5 bound is imposed because there is no single sampling step, and no hidden normalization by mean/minimum/maximum sample spacing changes these units.

Invalid static shape/axis or parameter syntax/type fails with InvalidArgument/InvalidDomain; dtype/shape mismatch uses TypeMismatch.

## Mathematics

The continuous source is the piecewise-linear function through positions/values. Convolution integrates coordinate length, not sample count. Between p_j,p_(j+1), reconstruction is the exact affine function through v_j and v_(j+1).

Boundary is defined over the continuous domain [positions[0],positions[K-1]]. Let a=p_0,b=p_(K-1),D=b-a as an exact real difference. Reflect mirrors the piecewise-linear signal at domain endpoints (triangular folding of (x-a) modulo 2D into [0,D]); replicate extends endpoint values (nearest endpoint outside [a,b]); zero is +0 outside; wrap uses (x-a) modulo D into [0,D), with the domain length as period. Wrap permits different first/last values and the resulting seam jump; no extra connecting segment is invented. Endpoint values at an isolated seam have zero measure in the convolution integral. Arithmetic for x+/-R, periods and coordinate folds does not overflow merely because an intermediate Float64 value would be unrepresentable; finite inputs retain their exact values.

The mathematical result at each source position x is

integral_{-R}^{R} k(s)*extended_f(x-s) ds / integral_{-R}^{R} k(s) ds.

Strict correctly rounds that complete integral quotient at output. The exact piecewise-linear reconstruction and mathematical kernel define the reference; a high-density uniform resampling followed by discrete filtering is not the strict reference. Boundary handling retains full-kernel normalization.

For support R, use the window formulas of [the uniform contract](CRV-11_uniform_lowpass_contract.md) with continuous u=s/R. Windowed sinc has k(s)=sincpi(2*cutoff*s)*w(s/R), |s|<=R; Gaussian has k(s)=exp(-s*s/(2*sigma*sigma)), |s|<=R. All are zero outside. Constants are exact rationals and pi/I0/exp are mathematical values. Normalization uses the exact integral over the full support, not the discrete tap sum. Positive normalization and the numeric support/cutoff/sigma/beta domains are unchecked caller premises, with no separate normalizer-domain validation.

Accelerated Apple Silicon and x86-64 keys satisfy the shared final FP32 bound relative to strict.

When every participating finite source/extension sample has identical bits, that constant is returned including -0. Otherwise exact zero is +0 and nonzero underflow keeps the mathematical sign. Virtual zero padding contributes +0 and may prevent the constant-bit result near boundaries.

## Behavior

The output at position x depends on the piecewise-linear signal within distance R of x, after boundary extension, and so on the values at the positions bracketing that support along the filtered axis; other-axis coordinates are independent. Which intervals contribute depends on the position values. Isolated contacts of the support with a knot add no endpoint contribution, and coefficient cancellation does not omit an endpoint from its piece. Region and tile results equal the corresponding slice of the whole result.

Finite strictly increasing positions, finite contributing source endpoint values and finite representable selected outputs are unchecked caller preconditions; no helper or kernel adds a numerical-domain validation pass. The continuous real-signal contract governs every positive-length reconstructed segment.

## Acceptance

Expected values come from the exact piecewise-linear reconstruction and certified continuous convolution. Finite constants under reflect/replicate/wrap are exactly preserved. If an affine signal's requested support remains within its reconstruction domain, an even normalized kernel returns its center value exactly. Example: positions=[0,0.75,2], values=[1,2.5,5], support_radius=0.5, query at the existing middle position returns 2.5 for every selected kernel. Inserting an additional collinear knot does not alter the reconstructed continuous function or this result; an equal-weight irregular sample average would fail that test.

A nontrivial boundary fixture is positions=[0,1], values=[2,2], support_radius=0.5: zero boundary has strict result [1,1] for every defined even kernel; accelerated uses the four-step bound. Reflect/replicate/wrap preserve [2,2] exactly in all profiles. Exactly half the kernel integral sees the constant source at each zero-padded endpoint. Replacing values with the smallest positive subnormal of its dtype gives an exact half-subnormal endpoint value: strict ties-to-even returns +0, and accelerated must match that zero classification. With source values three times the smallest positive subnormal, the exact endpoint is 1.5 times that minimum; strict returns twice the minimum by ties-to-even, and accelerated must remain nonzero within its four-step bound. The latter case distinguishes gradual underflow from flushing every subnormal result to zero.

Test impulses as piecewise-linear hats, strongly nonuniform gaps, kernel support touches, exact affine cancellation, wrap seam jumps, multiple reflection periods, zero boundaries, both dtypes, mixed position precision, subnormal/extreme parameters, valid finite endpoint extremes, local negative-zero support and representable output boundaries. Check accelerated 4-ULP/classification/constant obligations against the same continuous reference.

Frequency response applies to the specified continuous kernel and reconstruction; there is no single input Nyquist implied by nonuniform positions. This low-pass cannot recover unknown intersample content or undo aliasing already present in the source. A downsampling workflow must independently measure its reconstruction/ filter/resampling error and attenuation requirements. Uniform and nonuniform operators need not agree even at equally spaced positions: one filters discrete samples and the other filters their continuous linear reconstruction.

## References

- [Resampling family](CRV-11_resample_signal.md).
- [Uniform counterpart](CRV-11_uniform_lowpass_contract.md).
