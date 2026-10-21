---
spec_schema_version: 1
id: CRV-11E5
parent_id: CRV-11
function: lowpass_uniform_gaussian
proposed_operation_keys:
  - curve.lowpass_uniform_gaussian_strict
  - curve.lowpass_uniform_gaussian_accelerated_apple_silicon
  - curve.lowpass_uniform_gaussian_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-11E5: lowpass_uniform_gaussian

The dynamic inputs inherit the [family Result tensor-port contract](CRV-11_uniform_lowpass_contract.md): each port is a Result with exactly one tensor member  under any structurally valid schema id/version/member key. Shapes use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and kernel

The [uniform low-pass contract](CRV-11_uniform_lowpass_contract.md) is normative.
Dynamic `input` is a single-tensor Result; use its full `sample_shape()` (rank 1..8, positive extents/count <=2^40) and Float32/Float64 dtype. Output port `values` is a `photospider.tensor` v1/member `samples` Result preserving shape/dtype with no attached domain description. Static axis selects the
filtered dimension; integer radius=1..4096 sets symmetric support -R..R.
boundary is reflect/replicate/zero/wrap, default non-endpoint-repeating reflect.
Required kernel-specific static Float64 parameters are sigma.
Use exactly the gaussian formula in the shared contract, with full-kernel DC
normalization, no rounded coefficient reference and no output-position shift.
sigma>0 is in sample units and radius truncates the Gaussian; no cutoff parameter is accepted.

All relevant parameters are explicit; no target sample-rate inference occurs.

## Execution and numerical obligations

Strict rounds the exact normalized sum once; accelerated final error <=4 ULP,
with strict classification/zero/sign agreement and strict fallback.
Finite constants retain bits when all contributing extended samples match.
Follow the shared IEEE NaN/Inf ordering and skip only mathematical zero taps.
Do not drop underflowed approximate coefficients from logical dependencies.

All three keys use the [fixed execution and resource contract](CRV-11_uniform_lowpass_contract.md#demand-mapping-resources-and-error-contract). Each selected output tile has one CPU task with the exact mapped nonzero-tap source closure and private preallocated arithmetic scratch. Only selected outputs are computed and allocated; global coordinates and IEEE tap order are preserved. Empty selection emits no task. Preparation checks the complete invocation signature, and the executor publishes the call after its writes succeed and retire.

## Acceptance and implementation status

Use the shared certified coefficient/whole-sum oracle, impulse/DC/sine response,
both dtypes and all CPU profiles, zero taps, signed zero and IEEE exceptions.
Test both full signals and sparse/ROI requests, short signals, every boundary,
affine views, exact source-tile closure, low budgets, cancellation and post-context owners.
The frequency response and target-downsampling attenuation require independent
measurement; this primitive never claims ideal cutoff or zero aliasing.

The maintained public workflow binds input, axis/radius/boundary and sigma,
then requests values using the selected operation key; this Proposed specification
does not claim ideal cutoff or zero-aliasing behavior.

## Maintained implementation and validation

This primitive uses the uniform family's fixed CPU adapter. Exact tap support precedes numerical evaluation. Accelerated keys prepare certified coefficient enclosures once per selected tile invocation and propagate convolution/normalization error to the final output gate. Numerical uncertainty uses strict whole-sum evaluation; operational failures return directly. Special-value and constant shortcuts retain the positive-normalizer proof. Certified precision is bounded to 128..4096 bits; unresolved capacity or rounding may return `ResourceExhausted`. See [CRV-11 umbrella](CRV-11_resample_signal.md).
