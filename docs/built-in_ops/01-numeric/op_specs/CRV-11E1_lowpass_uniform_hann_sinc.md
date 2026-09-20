---
spec_schema_version: 1
id: CRV-11E1
parent_id: CRV-11
function: lowpass_uniform_hann_sinc
proposed_operation_keys:
  - curve.lowpass_uniform_hann_sinc_strict
  - curve.lowpass_uniform_hann_sinc_accelerated_apple_silicon
  - curve.lowpass_uniform_hann_sinc_accelerated_x86_64
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

# CRV-11E1: lowpass_uniform_hann_sinc

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
Required kernel-specific static Float64 parameters are cutoff.
Use exactly the hann_sinc formula in the shared contract, with full-kernel DC
normalization, no rounded coefficient reference and no output-position shift.
cutoff is in cycles/sample, strictly between 0 and 0.5, and denotes the ideal sinc parameter.

All relevant parameters are explicit; no target sample-rate inference occurs.

## Execution and numerical obligations

Strict rounds the exact normalized sum once; accelerated final error <=4 ULP,
with strict classification/zero/sign agreement and reported strict fallback.
Finite constants retain bits when all contributing extended samples match.
Follow the shared IEEE NaN/Inf ordering and skip only mathematical zero taps.
Do not drop underflowed approximate coefficients from logical dependencies.

Inherit exact stencil Data/Validation and dirty maps, boundary mapping including
N=1, normal source/typed closures, immutable Region origins/owners, source strides,
host coefficient/read-window/scratch accounting, bounded cancellation and cache-off.
Basic M-output work is O(M*(2R+1)) plus certified arithmetic; budgets include
precision refinement and scratch overlap. Inherit the shared error phases and
categories; no partial failed observation is published.

## Acceptance and implementation status

Use the shared certified coefficient/whole-sum oracle, impulse/DC/sine response,
both dtypes and all CPU profiles, zero taps, signed zero and IEEE exceptions.
Test both full signals and sparse/ROI requests, short signals, every boundary,
strides, exact dirty support, low budgets, cancellation and post-context owners.
The frequency response and target-downsampling attenuation require independent
measurement; this primitive never claims ideal cutoff or zero aliasing.

The maintained public workflow binds input, axis/radius/boundary and cutoff,
then requests values using the selected operation key. See the shared workflow link below for the maintained invocation, commands and
current evidence; this Proposed specification does not claim ideal cutoff or
zero-aliasing behavior.

## Maintained implementation and validation

This primitive is registered in the five-kernel uniform low-pass family.
Exact tap support precedes numerical evaluation. Accelerated keys cache certified
coefficient enclosures per continuation and propagate convolution/normalization
error to the final output gate. Unresolved coefficients or outputs use strict
whole-sum evaluation, with actual fallback diagnostics. Support and special-value
shortcuts remain exact.
Certified precision is bounded to 128..4096 bits; unresolved
capacity or rounding may return `ResourceExhausted`. See the [shared workflow](../../../../examples/numeric_workflow/README.md#uniform-lowpass)
and [CRV-11 umbrella](CRV-11_resample_signal.md). Native Clang21 Strict/Apple and WSL Clang18 Strict/AVX2 passed the
shared public manual groups and independent numerical references. The linked
workflow records exact counts, commands and installed-consumer checks.
