---
spec_schema_version: 1
id: RES-13C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: RES-13
function: flat_field_correct
proposed_operation_keys:
- restoration.flat_field_correct_strict
- restoration.flat_field_correct_accelerated_apple_silicon
- restoration.flat_field_correct_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# RES-13C: flat_field_correct

Correction from measured flat and dark fields.

Inherits the [RES-13 family contract](RES-13_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN, underflow, and accelerated error baselines are inherited and may not be weakened.

## Interface

Operation keys: `restoration.flat_field_correct_strict`, `restoration.flat_field_correct_accelerated_apple_silicon`, `restoration.flat_field_correct_accelerated_x86_64`.

### Ports

`input, dark, flat, flat_dark, valid_mask` → `corrected, valid`; calibration planes have matching shapes or explicit broadcasting. valid_mask is UInt8 with values exactly 0 or 1. Calibration sample values are consumed only where valid_mask=1; on that branch, dark, flat, and flat_dark must be finite. The exact difference d=flat-flat_dark must be greater than zero or the explicit invalid policy applies. Finite calibration values on that branch are an unchecked caller premise; nonfinite calibration inputs have no production diagnostic guarantee.

### Parameters

Finite reference_gain g > 0 in input units, supplied by external calibration or statistics over flat-flat_dark; no hidden global mean is computed. `invalid=copy_input|zero|error` (error is illustrative, not a default). Axes do not affect pointwise calculation.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematics

Numeric classification: **E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the meanings of E/B/S.

Compute d=flat-flat_dark exactly. For mask=1 and d>0, produce `RN_t((input-dark)*g/d)` and valid=1; otherwise apply the selected invalid policy and set valid=0. With mask=0, do not read calibration or dark data; the copy branch reads only input. Do not silently repair d≤0 with an epsilon.

## Behavior

Pointwise. Calibration values are read only where the mask is 1. The gain is an explicit scalar parameter; no whole-field statistic is computed.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

For input=10, dark=2, flat=6, flat_dark=2, and g=4, the result is 8. Exercise zero and negative flat-net policies. A zero mask does not read poisoned calibration data.

## References

[S24 · scikit-image rolling-ball example](../research-sources.md#s24)
