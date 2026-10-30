---
spec_schema_version: 1
id: NUM-12B
parent_id: NUM-12
function: quantile
proposed_operation_keys:
  - numeric.quantile_strict
  - numeric.quantile_accelerated_apple_silicon
  - numeric.quantile_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-12B: quantile

One linear quantile per logical line. Inherits the [NUM baseline](NUM_common_contract.md) for registration and shared rules.

## Interface

The `input` port is a `Result` containing exactly one tensor member at index zero, with any `ResultTensorSpec::key`. Supported source types are UInt8, Int64, Float32 and Float64. The source `sample_shape()` includes declared batch axes. The `q` port is another `Result` with exactly one tensor member at index zero and any member key; its `sample_shape()` is exactly `[1]` and its element type is Float32 or Float64.

| Parameter | Type and domain | Default |
| --- | --- | --- |
| `axis` | Int64 in `0..rank-1`, measured against the full source sample shape | Required |
| `dtype` | String `float32` or `float64` | Required |

Active q is finite in `[0,1]` as a caller numerical precondition; no diagnostic is guaranteed outside it. A malformed static parameter returns InvalidArgument; an incompatible schema returns TypeMismatch.

The `values` output is a `Result` with schema `photospider.tensor` and tensor key `samples`. It has the selected output dtype and keeps the source rank and shape, with the selected axis extent set to 1. Batch axes become ordinary output axes; output batch topology is empty. The source rank is 1..8 and its logical element count is at most 2^40.

## Mathematics

For an active line of length N, let `x[0..N-1]` be its samples in stable ascending numerical order. Decode q as an exact binary rational and form

$$
h=(N-1)q,\qquad j=\lfloor h\rfloor,\qquad w=h-j.
$$

If `w == 0`, the result selects `x[j]`. Otherwise

$$
y=(1-w)x[j]+wx[j+1].
$$

This handles q=1 without reading beyond `x[N-1]`. Integer samples keep exact integer values through ordering and interpolation; they are not first converted to floating point. The exact finite selection or interpolation is rounded once to the output dtype in all profiles, using exact final RN rounding for this dyadic denominator. There is no approximate quantile sketch or rank-error tolerance.

NaNs, infinities and zero signs. The complete source line is ordered and checked for NaNs before the rank is selected, including for q=0 or q=1. If a line contains a NaN, the first NaN in original axis order is returned, quieted and converted to `dtype` under the [reduction NaN payload rule](NUM-11_reduction_contract.md). Stable sorting keeps NaNs after non-NaNs in their original order. The q precondition applies even when the source contains NaNs.

When `w == 0`, the selected sample is converted directly, preserving its signed zero and correctly converting a selected infinity. For `0 < w < 1`, two same-sign infinities or one infinity paired with a finite value yield that signed infinity. Opposite-sign infinities yield the fixed positive quiet NaN. Two negative-zero interpolation endpoints produce negative zero; all other exact zero mixtures produce positive zero. Nonzero exact results that underflow keep their mathematical sign. Infinities outside the selected pair do not by themselves produce NaN. Finite source values can round to signed infinity when narrowing to Float32. All source IEEE bit patterns are supported.

## Behavior

Each output sample depends on the complete source line along `axis`, even for q=0 or q=1. For L>1 the q scalar is also read. For L=1 the q shape and dtype are still checked but the q payload and its producer do not participate. Empty selection reads nothing. Selected outputs share one call: an operational failure prevents successful publication of that call's outputs.

## Acceptance

Cases: integer endpoints near Int64 limits, q=0 and q=1, -0, subnormal q, adjacent binary values around integral ranks, both output dtypes, Float64-to-Float32 overflow, first-source NaN priority, same/opposite infinities, signed-zero ties and underflow, non-last axes, length-one q producer omission. The reference is an independent exact Fraction evaluation.
