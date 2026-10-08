---
spec_schema_version: 1
id: CRV-11E3
parent_id: CRV-11
function: lowpass_uniform_blackman_sinc
proposed_operation_keys:
  - curve.lowpass_uniform_blackman_sinc_strict
  - curve.lowpass_uniform_blackman_sinc_accelerated_apple_silicon
  - curve.lowpass_uniform_blackman_sinc_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
repository_branch: ops-specs
verification_status: focused_result_validation_passed
repository_commit: current working tree
---

# CRV-11E3: lowpass_uniform_blackman_sinc

The dynamic inputs inherit the [family Result tensor-port contract](CRV-11_uniform_lowpass_contract.md): each port is a Result with exactly one tensor member and no fields under any structurally valid schema id/version/member key. Shapes use complete `sample_shape()` values, including batch axes.



Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and kernel

The [uniform low-pass contract](CRV-11_uniform_lowpass_contract.md) is normative.
Dynamic `input` is a single-tensor Result; use its full `sample_shape()` (rank 1..8, positive extents/count <=2^40) and Float32/Float64 dtype. Output port `values` is a `photospider.tensor` v1/member `samples` Result preserving shape/dtype with generic facets. Static axis selects the
filtered dimension; integer radius=1..4096 sets symmetric support -R..R.
boundary is reflect/replicate/zero/wrap, default non-endpoint-repeating reflect.
Required kernel-specific static Float64 parameters are cutoff.
Use exactly the blackman_sinc formula in the shared contract, with full-kernel DC
normalization, no rounded coefficient reference and no output-position shift.
cutoff is in cycles/sample, strictly between 0 and 0.5, and denotes the ideal sinc parameter.

All relevant parameters are explicit; no target sample-rate inference occurs.

## Execution and numerical obligations

Strict rounds the exact normalized sum once; accelerated final error <=4 ULP,
with strict classification/zero/sign agreement and strict fallback.
Finite constants retain bits when all contributing extended samples match.
Follow the shared IEEE NaN/Inf ordering and skip only mathematical zero taps.
Do not drop underflowed approximate coefficients from logical dependencies.

All three formal keys use Whole Result programs. Nonempty requests declare Data,
Validation and Descriptor needs (role 13) for the complete input member. Typed and
upstream validation covers the full tensor; authorized windows feed the callback
directly without collecting or copying full input. Whole computes all outputs.
Input changes invalidate complete recorded output demand; empty demand reads no
payload after static preflight. The Result writer publishes the complete same-
shape/dtype output in one transaction, with full coverage and global coordinates.
The Root accounts source windows, the full N*sizeof(dtype) output, and bounded
coefficient/tap workspace. Legal strides remain supported; the output owner
survives context teardown. Failure or cancellation publishes no partial output
and releases temporary state. The shared contract defines the unchanged
mathematical tap selection, boundary mapping, NaN/Inf order and zero signs.

## Acceptance and implementation status

Use the shared certified coefficient/whole-sum oracle, impulse/DC/sine response,
both dtypes and all CPU profiles, zero taps, signed zero and IEEE exceptions.
Test both full signals and sparse/ROI requests, short signals, every boundary,
strides, complete dirty support, low budgets, cancellation and post-context owners.
The frequency response and target-downsampling attenuation require independent
measurement; this primitive never claims ideal cutoff or zero aliasing.

The maintained public workflow binds input, axis/radius/boundary and cutoff,
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
and [CRV-11 umbrella](CRV-11_resample_signal.md). The focused Result CTest, Strict/Apple manual workflows, independent Result oracles and installed consumer pass. Historical performance measurements are from the earlier Value implementation; no Result performance or x86 numerical execution was run. See the shared workflow for exact coverage and commands.
