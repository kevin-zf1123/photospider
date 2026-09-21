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
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-11E5: lowpass_uniform_gaussian

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and kernel

The [uniform low-pass contract](CRV-11_uniform_lowpass_contract.md) is normative.
Dynamic input is input, Float32/Float64 rank 1..8, positive extents/count <=2^40.
Named values preserves shape/dtype with generic facets. Static axis selects the
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

All three formal profile keys use Whole. Nonempty requests collect the complete
input and compute one dense output of the same shape/dtype. Any input edit
invalidates all outputs; full typed/upstream validation can fail outside delivered
Q. Mathematical tap selection, boundary mapping, NaN/Inf order and zero signs
remain unchanged. Zero taps remain unused numerical operands. Empty reads nothing.
For total N elements work is O(N*(2R+1)) plus certified arithmetic, and output
storage is N*sizeof(dtype), alongside complete collected input and bounded
coefficient/tap workspace. Failure/cancellation publishes no partial output and
releases temporaries. Inherit legal strides, owned output, cache-off and managed
work/capacity checks from the shared contract.

## Acceptance and implementation status

Use the shared certified coefficient/whole-sum oracle, impulse/DC/sine response,
both dtypes and all CPU profiles, zero taps, signed zero and IEEE exceptions.
Test both full signals and sparse/ROI requests, short signals, every boundary,
strides, complete dirty support, low budgets, cancellation and post-context owners.
The frequency response and target-downsampling attenuation require independent
measurement; this primitive never claims ideal cutoff or zero aliasing.

The maintained public workflow binds input, axis/radius/boundary and sigma,
then requests values using the selected operation key. See the shared workflow link below for the maintained invocation, commands and
current evidence; this Proposed specification does not claim ideal cutoff or
zero-aliasing behavior.

## Maintained implementation and validation

This primitive is registered in the five-kernel uniform low-pass family.
Exact tap support precedes numerical evaluation. Accelerated keys cache certified
coefficient enclosures per Whole invocation and propagate convolution/normalization
error to the final output gate. Unresolved coefficients or outputs use strict
whole-sum evaluation, with Whole fallback counters unavailable. Support and special-value
shortcuts remain exact.
Certified precision is bounded to 128..4096 bits; unresolved
capacity or rounding may return `ResourceExhausted`. See the [shared workflow](../../../../examples/numeric_workflow/README.md#uniform-lowpass)
and [CRV-11 umbrella](CRV-11_resample_signal.md). Native Clang21 Strict/Apple validation for the Whole revision is recorded in
that workflow and the math implementation notes. WSL/AVX2 and installed-package
consumers have not been rerun. Whole numerical/fallback counters are N/A.
