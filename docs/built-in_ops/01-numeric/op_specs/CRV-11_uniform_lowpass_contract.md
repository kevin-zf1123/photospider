---
spec_schema_version: 1
id: CRV-11-UNIFORM
parent_id: CRV-11
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-11: uniformly sampled low-pass family

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

Five independent kernels are windowed sinc (Hann/Hamming/Blackman/Kaiser) and
truncated Gaussian. Input Float32/Float64 arrays are filtered along a static axis;
other axes identify independent signals/channels. Output retains shape/dtype as
generic numeric data. A static integer radius is 1..4096, producing a centered
symmetric 2*radius+1-tap kernel. Output remains aligned to input positions.
Coordinates are in sample units and frequency is cycles/sample; no dynamic
start/step port is required.

All kernel parameters are static and explicitly supplied. Sinc windows require
finite 0<cutoff<0.5 cycles/sample. Kaiser additionally requires finite beta>=0.
Gaussian requires finite sigma>0 in samples and uses radius for truncation.
These Float64 numerical domains are unchecked caller premises at authoring,
preparation and invocation. The integer radius limit is checked because it fixes
support and scratch structure. No cutoff/support inference from a target
resampling rate occurs.

Static boundary is reflect/replicate/zero/wrap, default reflect with no endpoint
repetition. Preserve output length and the same kernel at boundaries; do not
renormalize against only in-range samples or truncate the stencil. Reflect uses
period 2*(N-1) when N>1; for N=1 every reflected index maps to zero. Replicate
clamps indices; wrap takes Euclidean modulo N; zero uses virtual +0 out of range.

The mathematical output is sum_j k[j]*extended_input[i+j] / sum_j k[j], j=-R..R,
with exact kernel coefficients and only one final rounding to input/output dtype.
Strict does not first round coefficients to Float64. The full-kernel normalization
fixes the exact DC gain at one. Zero-boundary extensions may still alter constant
finite signals near the edge, as specified by the boundary operator.

Each kernel has strict, accelerated_apple_silicon and accelerated_x86_64 keys.
Accelerated final nonzero finite output error is <=4 ULP relative to strict,
with strict NaN/Inf/zero classification and signs. Fall back to strict when the guarantee is unavailable. Preserve finite constant signals
exactly under non-zero-padding boundaries; approximate kernel normalization
cannot introduce DC drift. Host budget exhaustion remains explicit failure.

Skip exact mathematical zero coefficients entirely. Numerical operands are samples reached by
nonzero taps after boundary mapping. Lowering closes their union on the source tile grid; extra samples within those physical tiles are not numerical operands. Input NaN propagates by the first tap in
offset order -R..R, preserving payload/sign and quieting sNaN. Otherwise combine
infinite contributions with coefficient signs: both signs yield canonical quiet
NaN, one sign yields that infinity. Generated canonical NaN is positive quiet
NaN with bits 0x7fc00000 (Float32) or 0x7ff8000000000000 (Float64). Quiet an
input signaling NaN by setting its quiet bit, preserving sign and payload.
These are successful IEEE results. Do not
merge reflected duplicate taps in a way that changes specified NaN/Inf behavior.

When all finite contributing extended samples have identical bits, return that
constant including -0; all contributing samples are still read. Virtual
zero padding has +0 bits. Otherwise an exact zero result is +0 and nonzero
underflow retains sign. Infinite/NaN inputs use the explicit aggregation rules,
not the finite-constant shortcut. Special-value, constant and exact-symmetry
shortcuts trust positive full normalization and need no separate normalizer
calculation. General sums retain denominator enclosure refinement for division.

## Kernel definitions and static parameters

Write R=radius, u=j/R, S(z)=sin(pi*z)/(pi*z) with S(0)=1. For four windowed
sinc kernels k[j]=S(2*cutoff*j)*w(u), -R<=j<=R. The common factor 2*cutoff
cancels in full-kernel normalization. Use mathematical pi and exact coefficients:

| Independent kernel | w(u), or direct k[j] | Extra static parameters |
| --- | --- | --- |
| hann_sinc | (1+cos(pi*u))/2 | cutoff |
| hamming_sinc | 0.54+0.46*cos(pi*u) | cutoff |
| blackman_sinc | 0.42+0.5*cos(pi*u)+0.08*cos(2*pi*u) | cutoff |
| kaiser_sinc | I0(beta*sqrt(1-u*u))/I0(beta) | cutoff,beta |
| gaussian | k[j]=exp(-j*j/(2*sigma*sigma)) | sigma |

Decimal window constants are exact decimal rationals; these are symmetric windows,
not periodic analysis windows. I0 is the modified Bessel function of order zero.
At beta=0 the Kaiser window is one. Exact Hann/Blackman endpoints and sinc integer
zeros are recognized before reading inputs. Gaussian coefficients are mathematically
positive even if a rounded coefficient implementation would underflow; such taps
cannot be dropped from source demand or IEEE classification.

Static axis and radius are integers; boundary is String. cutoff/beta/sigma are
Float64 interpreted as their actual binary values. All are required for the
relevant kernel except boundary defaults reflect in constructors. No unused
kernel parameter, output dtype, sample spacing or automatic quality parameter is
accepted. The only dynamic input is input; the named output is values.

The dynamic port `input` is a Result containing exactly one tensor member and no fields. Any structurally valid schema id/version/member key is accepted. Use its complete `sample_shape()`, including batch axes, for rank, axis and extent checks. Input rank is 1..8, Float32/64, positive extents and a product at most 2^40.

The `values` output port is an immutable Result using `photospider.tensor` v1/member `samples`, with the same full shape and dtype and no attached domain description. The output retains its backing owners; the schema carries no operation resource configuration. Its full shape consists of ordinary axes with zero atomic trailing axes; input batch/atomic grouping is not retained. Only selected output tiles are allocated and published, with global coordinates.

Input rank is 1..8 with positive extents and count <=2^40; axis is a nonnegative
index below rank. Axis length may be one or smaller than the kernel. For N=1,
reflect/replicate/wrap repeat the sole sample; zero retains virtual +0 padding.
Source/output dtype is Float32 or Float64, unchanged, with no attached domain description.

The normalizer is the exact full tap sum and is positive as an unchecked
numerical premise. Interval refinement continues until division and final
rounding are determined; inability to complete that computation within budgets
is ResourceExhausted. There is no nonpositive-normalizer admission check. Numerical
overflow in an intermediate I0, squared parameter or naive coefficient calculation
does not define an invalid kernel. Avoid it or refine at adequate precision.

## Demand, mapping, resources and error contract

All 15 profile keys lower to one ABI 8 CPU task per selected output tile. Each task declares the union of source tiles reached by its exact nonzero taps. Lowering maps the selected axis interval through the static boundary rule: zero clips it, replicate clamps it, wrap can split it across the seam, and reflect folds it at turning points. Each mapped interval closes on the source's own spatial/atomic tile grid; batch coordinates remain explicit. Overlapping closures merge before enumeration. Logical tap order and repeated reflected samples remain unchanged during arithmetic.

Preparation verifies the canonical output region, complete ordered read set, dtype, empty task parameters and exact scratch size. The invocation borrows affine source views, including negative/zero strides and unaligned addresses, and writes its preallocated dense output tile. Output axes use the complete input sample shape as ordinary axes. Empty selection performs static preparation and emits no task or payload read; it does not evaluate the numerical normalizer.

For M selected output elements, work is O(M*(2R+1)) plus certified arithmetic. Each invocation owns fixed arithmetic state, 8*(2R+1) bytes of sample words and, for accelerated profiles, 16*(R+1) bytes of coefficient enclosures. Coefficients are prepared once per selected output tile invocation. Output storage scales with selected tiles; source-grid closure can require extra physical samples. Lowering counts source tile records before enumeration and rejects a closure exceeding its record budget. Immutable preparation can be reused by concurrent Runs, each with private output and scratch storage. Escaped Results retain their storage owners.

Cancellation is checked during coefficient/refinement work and at least every 64 tap contributions or simpler outputs. Operational cancellation/resource failures propagate directly before accepting a shortcut or retrying strict arithmetic. Numerical uncertainty alone can trigger strict refinement. The executor publishes a successful call only after all its declared writes succeed and the call retires; a failed call publishes no partial Result.

Invalid structural shape/axis/radius or parameter syntax/type fails preparation with InvalidArgument/InvalidDomain, and incompatible dtype uses TypeMismatch. Numeric cutoff/sigma/beta domains and positive normalization are unchecked caller premises. Inability to refine division or unique final rounding within arithmetic capacity is ResourceExhausted. Source NaN/Inf and final arithmetic overflow are successful IEEE values under the rules above. Source binding, upstream, cancellation and resource failures retain the common execution categories.

## Numerical and antialias acceptance

Use independent high-precision/certified coefficient and complete-sum oracles,
with symbolic symmetry/zero/constant handling for exact boundaries. A pipeline
using pre-rounded Float64 coefficients is not the strict reference. Test all
three profiles, both dtypes, extreme valid parameters, subnormal coefficients,
canceling sums, DC, impulse, phase alignment, NaN/Inf order and zero padding.
Hann R=1 is identity because only the center coefficient is nonzero. Hann R=2,
cutoff=0.25 has weights proportional to [0,1/pi,1,1/pi,0]; an interior unit
impulse returns the correctly rounded pi/(pi+2) at its center. These statements
refer to the defined mathematical kernel, not a third-party binary match.

Additional strict impulse fixtures use input=[0,0,1,0,0], radius=2 and output
index 2. Sinc kernels use cutoff=0.25; Kaiser uses beta=0, Gaussian sigma=1.
The following expressions are rounded once to the output dtype; accelerated
results use the stated final four-step bound, not forced bit equality.

| Kernel | Exact center result before final rounding |
| --- | --- |
| Hann sinc | pi/(pi+2) |
| Hamming sinc | pi/(pi+2.16) |
| Blackman sinc | pi/(pi+1.36) |
| Kaiser sinc, beta=0 | pi/(pi+4) |
| Gaussian, sigma=1 | 1/(1+2*exp(-1/2)+2*exp(-2)) |

Each also has the bit-exact constant fixture under reflect/replicate/wrap.
These identity and nontrivial fixtures apply to the corresponding child specs.

The discrete response is H(f)=sum_j k[j]*exp(-2*pi*i*f*j)/sum_j k[j]. Verify
it against independent frequency-response computation and deterministic sinusoids
in the interior/periodic case. cutoff is the ideal sinc-design parameter; finite
radius creates a transition band and nonzero stopband leakage. It is not a
universal measured -3/-6 dB or guaranteed-alias-rejection boundary. Gaussian also
has no brick-wall stopband. A downsampling workflow must separately select radius/
kernel parameters and verify attenuation above its target Nyquist for its quality
requirement; this primitive does not infer that target or guarantee zero aliasing.

Verify full/ROI/disjoint numerical equivalence, exact boundary mathematics, zero-tap omission, source-grid closures, N=1, short signals, affine views, source/result lifetime, low budgets and cancellation. Exercise sparse large logical tensors, concurrent Runs, floating environment restoration, fixed scratch bounds and allocation-free invocation. Use a temporary public workflow with the input and required statics, request
values through Compiler/ExecutionContext, and check independent expected outputs.

Primary method references: [SciPy firwin](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.firwin.html),
[Blackman](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.windows.blackman.html),
[Hamming](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.windows.hamming.html),
[Kaiser](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.windows.kaiser.html).
These identify design families; the formulas and numerical contract above govern
this implementation and do not claim SciPy floating-point parity.

- [Resampling family](CRV-11_resample_signal.md).
- [Operator template](../../00-foundation/spec-template.md).

## Maintained implementation and validation

This shared contract covers five uniform kernels and 15 profile keys. `lowpass_uniform.cpp` stores immutable parameters and emits `uniform-lowpass-unchecked-v2` tasks. `UniformLowpassMath` uses `CheckedLowpassKernel` and explicit failure state for certified sums. Accelerated profiles prepare 128-bit coefficient enclosures in invocation scratch, then bound the complete hardware convolution and normalization before final-error acceptance. Numerical uncertainty dispatches strict convolution; cancellation/resource failures return directly. Certified strict precision is 128..4096 bits and may fail `ResourceExhausted`.

The independent directed-MPFR reference in `oracle/ops/numeric/crv11/uniform.py` imports wide rational numerators and denominators with outward rounding. Temporary public and direct invocation checks compare strict bits and accelerated final FP32 bounds independently. See [CRV-11 umbrella](CRV-11_resample_signal.md).
