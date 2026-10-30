---
spec_schema_version: 1
id: RES-14B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-14
function: dark_channel_transmission
proposed_operation_keys:
- restoration.dark_channel_transmission_strict
- restoration.dark_channel_transmission_accelerated_apple_silicon
- restoration.dark_channel_transmission_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# RES-14B: dark_channel_transmission

Dark-channel transmission estimation.

Inherits the [RES-14 family contract](RES-14_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN, underflow, and accelerated error baselines are inherited and may not be weakened.

## Interface

Operation keys: `restoration.dark_channel_transmission_strict`, `restoration.dark_channel_transmission_accelerated_apple_silicon`, `restoration.dark_channel_transmission_accelerated_x86_64`.

### Ports

The explicitly selected, linear opaque RGB `input` and Float64 `[3]` `airlight` → single-plane `transmission`. The selected color group has no associated alpha plane; reject alpha-associated structure at preflight without scanning alpha samples. Input RGB must be finite and nonnegative.

### Parameters

radius ≥ 0; omega ∈ [0,1] (0.95 is illustrative, not a default); boundary=truncate; axes/component_axis; explicit `input_description` and RGB `group` as in RES-14A. For omega>0, each consumed airlight component must be finite and >0. At omega=0, the exact constant-one branch consumes no input or airlight samples.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematics

Numeric classification: **E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the meanings of E/B/S.

`t(q)=RN_t(clamp(1-omega*min_(p∈Bq,c)(I(p,c)/A_c),0,1))`. Compare quotients exactly without prior rounding; the clamp is an explicit part of the estimation model. Refinement uses FIL-07A followed by an explicit clamp; it is not hidden in this node.

## Behavior

Each output sample depends on the radius-r window of the input and on all three airlight components. When omega=0, the result is the explicit constant 1 and input/airlight sample values are not read.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

If I=A at every neighbor, t=1-omega. omega=0 yields 1. For HDR input, a minimum ratio above 1 may clamp to 0. Positive finite A is a caller premise when consumed. The omega=0 bypass consumes no A samples.

## References

[S23 · He, Sun, Tang: Single Image Haze Removal](../research-sources.md#s23)
