---
spec_schema_version: 1
id: RES-13B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: RES-13
function: subtract_background
proposed_operation_keys:
- restoration.subtract_background_strict
- restoration.subtract_background_accelerated_apple_silicon
- restoration.subtract_background_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# RES-13B: subtract_background

Explicit background subtraction.

Inherits the [RES-13 family contract](RES-13_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN, underflow, and accelerated error baselines are inherited and may not be weakened.

## Interface

Operation keys: `restoration.subtract_background_strict`, `restoration.subtract_background_accelerated_apple_silicon`, `restoration.subtract_background_accelerated_x86_64`.

### Ports

Same-shape `input, background` → `corrected`.

### Parameters

Finite restore_offset (illustrative value 0); no clipping.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematics

Numeric classification: **E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the meanings of E/B/S.

Evaluate `RN_t(input-background+restore_offset)` as one exact expression followed by one RN. Brightness preservation is not automatic.

## Behavior

Pointwise operation over both inputs.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

When I=B, output is restore_offset. Preserve negative outputs. Retain accuracy for large common-mode values whose difference is small.

## References

[S24 · scikit-image rolling-ball example](../research-sources.md#s24)
