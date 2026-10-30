---
spec_schema_version: 1
id: NUM-06B
parent_id: NUM-06
function: remap_range
proposed_operation_keys:
  - numeric.remap_range_strict
  - numeric.remap_range_accelerated_apple_silicon
  - numeric.remap_range_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-06B: remap_range

Maps a source interval to a target interval and extrapolates linearly outside the source interval. Strict uses the exact reference formula. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Inherits the [NUM baseline](NUM_common_contract.md); explicit rules below take precedence.

## Interface

Inputs are five `Result` tensor members, in port order: `input`, `source_lower`, `source_upper`, `target_lower`, `target_upper`. Each contains exactly one tensor member at index zero, with any `ResultTensorSpec::key`. All five have identical `sample_shape()` (including every batch axis) and element type, Float32 or Float64. Input rank is 1..8 and the logical element count is at most 2^40. There are no static parameters, implicit casts or implicit broadcasts. Clipping is a separate clamp operation.

The output port is `values`, a `Result` with schema `photospider.tensor` and tensor key `samples`. It preserves the input element type and uses the complete input `sample_shape()` as the output cell shape; batch axes become ordinary output axes, and the output carries no domain description. Strict, Apple Silicon and x86-64 profile keys are provided.

## Mathematics

For finite ordinary inputs,

$$
y = t_0 + (x-s_0)\,\frac{t_1-t_0}{s_1-s_0}.
$$

The caller supplies four finite bounds with `source_lower < source_upper`. Target bounds may increase, decrease or be equal. These are unchecked numerical preconditions.

An input NaN is quieted with its sign and payload preserved. The remaining cases apply in this order:

1. Equal target bounds return the `target_lower` bits for every non-NaN input, including infinite inputs and opposite-sign target zeros.
2. An exact `source_lower` input returns the `target_lower` bits.
3. An exact `source_upper` input returns the `target_upper` bits.
4. An infinite input maps to the correctly signed infinity according to the exact slope sign.
5. A finite input evaluates the whole formula as an exact dyadic rational and rounds once to the output type.

A finite mathematical result that overflows returns the correctly signed infinity. Gradual underflow preserves the exact result sign; an exact non-special zero is positive zero. Endpoint selection preserves the supplied target endpoint bits.

Strict rounds the exact rational result directly. Accelerated profiles satisfy the [FP32 four-ULP contract](NUM_accelerated_contract.md) against it.

## Behavior

Each output sample depends on the five inputs at the same coordinate. All declared inputs remain dependencies. Empty selection reads nothing.

## Acceptance

Float64 input `[0,0.5,1,2]` with source bounds `[0,1]` and target bounds `[0,255]` (each broadcast explicitly to the input shape) gives `[0,127.5,255,510]`. Cases: both dtypes, reversed and equal target bounds, endpoint zero signs, extreme finite source widths, extrapolation, against an independent Fraction reference.
