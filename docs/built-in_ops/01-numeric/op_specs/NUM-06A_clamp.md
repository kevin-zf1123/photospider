---
spec_schema_version: 1
id: NUM-06A
parent_id: NUM-06
function: clamp
proposed_operation_keys:
  - numeric.clamp_strict
  - numeric.clamp_accelerated_apple_silicon
  - numeric.clamp_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-06A: clamp

Numeric profile: strict is the exact reference. Clamp comparisons and selected results are exact in every profile; floating arithmetic follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md) where applicable. Inherits the [NUM baseline](NUM_common_contract.md); explicit rules below take precedence.

## Interface

The three input ports are `input`, `lower` and `upper`, in that order. Each is a `Result` containing exactly one tensor member at index zero, with any `ResultTensorSpec::key`. The three members have identical `sample_shape()` (including every batch axis) and element type, one of UInt8, Int64, Float32 and Float64. Input rank is 1..8 and the logical element count is at most 2^40. There are no static limit parameters, implicit casts or implicit broadcasts.

The output port is `values`, a `Result` with schema `photospider.tensor` and tensor key `samples`. It preserves the input element type and uses the complete input `sample_shape()` as the output cell shape; batch axes become ordinary output axes, and the output carries no domain description. Incompatible dtype, sample shape, rank or element count is rejected before execution. Strict, Apple Silicon and x86-64 profile keys are provided.

## Mathematics

The caller supplies non-NaN bounds satisfying `lower <= upper` at each computed coordinate. Equal bounds and infinite floating bounds are valid. These are unchecked numerical preconditions.

With valid bounds, an input NaN is quieted with its sign and payload preserved. Otherwise the result is `lower` when `input < lower`, `upper` when `input > upper`, and the input bits when it lies in range. Comparisons treat signed zeros as equal, so an in-range `-0` remains `-0`; in particular `clamp(-0,+0,+0) = -0`. The operation is not defined by composing minimum and maximum. Integer order uses exact signed comparison, and selected values are not converted or rounded.

## Behavior

Each output sample depends on the three inputs at the same coordinate. All declared inputs remain dependencies. Empty selection reads nothing.

## Acceptance

Input `[-1, 0.5, 2]`, lower `[0,0,0]`, upper `[1,1,1]` gives `[0,0.5,1]`. Cases: supported dtypes, integer extrema, signed zero ties, infinite bounds, equal bounds, NaN input.
