---
spec_schema_version: 1
id: RES-14A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-14
function: dark_channel_airlight
proposed_operation_keys:
- restoration.dark_channel_airlight_strict
numeric_reference: order/select exact
oracle_scope: mathematical_reference
---

# RES-14A: dark_channel_airlight

Dark-channel and airlight estimation.

Inherits the [RES-14 family contract](RES-14_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN, underflow, and accelerated error baselines are inherited and may not be weakened.

## Interface

Operation key: `restoration.dark_channel_airlight_strict`.

### Ports

The explicitly selected, linear opaque RGB `input` with no associated alpha plane → single-plane `dark_channel` with the same H/W; Float64 `[3]` `airlight`; and Int64 `[2]` `selected_pixel`. Finite nonnegative RGB samples are caller premises; no numerical input scan is performed. Alpha-bearing groups fail structural preflight; do not scan alpha samples to infer opacity.

### Parameters

radius ≥ 0; top_fraction f ∈ (0,1] (0.001 is illustrative, not a default); axes/component_axis; boundary=truncate; `input_description` (canonical TensorDescription v4/v5 String) and an explicit RGB `group`.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematics

Numeric classification: **exact ordering/selection**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

D(q)=min_(p∈Bq,c) I(p,c). For airlight, select the k=max(1,ceil(f*H*W)) pixels with largest D, breaking ties by earlier row-major index. Among them, select one pixel with the largest exact RGB sum, again breaking ties by row-major order. A is that pixel’s RGB converted exactly to Float64; do not independently maximize each channel across candidates. When airlight is selected, callers guarantee that each component of this selected A is strictly positive. Production execution does not test this numerical premise or add epsilon; the premise does not constrain a dark-channel-only request.

## Behavior

The dark channel at q depends on the radius-r window. Airlight and selected_pixel depend on the complete input and dark map. A request for D alone does not require airlight estimation.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

For constant positive RGB, D equals the minimum channel, A equals that RGB, and the selected index is (0,0). An all-black input is valid for D alone; selecting A for that input violates the positive-airlight premise and has no production failure guarantee. Even if top_fraction<1/P, select one pixel.

## References

[S23 · He, Sun, Tang: Single Image Haze Removal](../research-sources.md#s23)
