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
---

# CRV-11E3: lowpass_uniform_blackman_sinc

The dynamic inputs follow the [family Result tensor-port contract](CRV-11_uniform_lowpass_contract.md): each port is a Result with exactly one tensor member under any structurally valid schema id/version/member key. Shapes use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Interface

The [uniform low-pass contract](CRV-11_uniform_lowpass_contract.md) is normative. Dynamic `input` is a single-tensor Result; its full `sample_shape()` has rank 1..8, positive extents/count <=2^40 and Float32/Float64 dtype. Output port `values` is a `photospider.tensor` v1/member `samples` Result preserving shape/dtype with no attached domain description. Static axis selects the filtered dimension; integer radius=1..4096 sets symmetric support -R..R. boundary is reflect/replicate/zero/wrap, default non-endpoint-repeating reflect. Required kernel-specific static Float64 parameters are cutoff. All relevant parameters are explicit; no target sample-rate inference occurs.

## Mathematics

The blackman_sinc formula of the shared contract applies, with full-kernel DC normalization, no rounded coefficient reference and no output-position shift. cutoff is in cycles/sample, strictly between 0 and 0.5, and denotes the ideal sinc parameter.

Strict rounds the exact normalized sum once; accelerated final error <=4 ULP, with strict classification/zero/sign agreement. Finite constants retain bits when all contributing extended samples match. The shared IEEE NaN/Inf ordering applies and only mathematical zero taps are skipped.

## Behavior

The output element depends on the input samples reached by its nonzero taps after boundary mapping, as in the shared contract; global coordinates and IEEE tap order are preserved. Region and tile results equal the corresponding slice of the whole result.

## Acceptance

Expected values come from the shared exact coefficient/whole-sum evaluation, with impulse/DC/sine response, both dtypes and all CPU profiles, zero taps, signed zero and IEEE exceptions. Full signals, sparse/ROI requests, short signals and every boundary are covered. The frequency response and target-downsampling attenuation require independent measurement; this primitive never claims ideal cutoff or zero aliasing.
