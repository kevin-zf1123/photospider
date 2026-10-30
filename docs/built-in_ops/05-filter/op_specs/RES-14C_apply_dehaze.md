---
spec_schema_version: 1
id: RES-14C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-14
function: apply_dehaze
proposed_operation_keys:
- restoration.apply_dehaze_strict
- restoration.apply_dehaze_accelerated_apple_silicon
- restoration.apply_dehaze_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# RES-14C: apply_dehaze

Application of the transmission model.

Inherits the [RES-14 family contract](RES-14_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN, underflow, and accelerated error baselines are inherited and may not be weakened.

## Interface

Operation keys: `restoration.apply_dehaze_strict`, `restoration.apply_dehaze_accelerated_apple_silicon`, `restoration.apply_dehaze_accelerated_x86_64`.

### Ports

The explicitly selected, linear, straight RGB group with no associated alpha plane, Float64 `[3]` airlight, and same-shape single-plane transmission → same-shape opaque RGB `output`. Reject alpha-associated structure at preflight without scanning alpha samples. Input RGB must be finite and nonnegative.

### Parameters

t_floor ∈ (0,1] (0.1 is illustrative, not a default); explicit `input_description` and RGB `group`. No automatic contrast, gamma, or clipping.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematics

Numeric classification: **E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the meanings of E/B/S.

Callers guarantee t∈[0,1]. Let te=max(t,t_floor), then `J_c=RN_t(A_c+(I_c-A_c)/te)`. At t=1, explicitly copy I without reading A samples and preserve its bits, under the caller premise that consumed I is finite and nonnegative. The output may be signed and may be HDR; the input’s nonnegative-value guarantee does not carry over.

## Behavior

For each requested color component, read only its I and t; when te<1, read the corresponding A_c. No other RGB component payload is needed. The selected group schema is checked statically under explicit caller-supplied color interpretation.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

For I=0.6, A=1, and t=0.5, output is 0.2. t=0 uses the floor; t=1 copies. Do not clip negative J. An R-only request does not read poisoned G/B values.

## References

[S23 · He, Sun, Tang: Single Image Haze Removal](../research-sources.md#s23)
