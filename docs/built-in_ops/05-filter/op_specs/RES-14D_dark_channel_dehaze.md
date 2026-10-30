---
spec_schema_version: 1
id: RES-14D
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-14
function: dark_channel_dehaze
proposed_operation_keys: []
numeric_reference: S
oracle_scope: composed_math_references
---

# RES-14D: dark_channel_dehaze

Complete dark-channel dehazing workflow.

Inherits the [RES-14 family contract](RES-14_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN, underflow, and accelerated error baselines are inherited and may not be weakened.

## Interface

### Ports

The explicitly selected, linear, opaque RGB `input` with no associated alpha plane → `output, transmission, airlight`, independently observable according to requested outputs. Reject alpha-associated structure at preflight; do not drop alpha or inspect its samples to infer opacity. RGB samples must be finite and nonnegative.

### Parameters

All parameters of stages A/B/C; refine=none|guided_scalar. For refinement, construct the guide explicitly from RGB; this profile defines guide=RN_t((R+G+B)/3) per pixel, where t is the main RGB/transmission dtype; this makes the guide and transmission valid same-dtype FIL-07A inputs. FIL-07A radius and epsilon are explicit. Explicitly clamp the guided result to [0,1] with RN_t.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematics

Numeric classification: **S**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the meanings of E/B/S.

A → B → optional guided filter plus clamp → C. Rounding is explicit at every stage. An airlight-only request does not run apply. Requesting output requires complete-input A, so the complete restoration is complete-input. The guide gray is the arithmetic mean defined by this profile; it is not claimed to be photometric luminance.

## Behavior

For airlight, demand complete-input A. Raw transmission combines A’s complete-input demand with its local demand. B can be local when supplied an existing A, but the complete D workflow is not a local filter.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

A constant nonblack scene may be amplified or distorted and serves as a prior-model negative case. Reject alpha-associated input during preflight; do not silently discard alpha.

## References

[S23 · He, Sun, Tang: Single Image Haze Removal](../research-sources.md#s23)
