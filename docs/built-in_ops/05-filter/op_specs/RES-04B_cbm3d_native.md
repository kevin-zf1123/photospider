---
spec_schema_version: 1
id: RES-04B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-04
function: cbm3d_native
proposed_operation_keys: []
numeric_reference: F
oracle_scope: mathematical_reference_pending
---

# RES-04B: cbm3d_native

Color CBM3D native algorithm contract.

Inherits the [RES-04 family contract](RES-04_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and this member define requirements for the native profile, whose detailed mathematical configuration remains unresolved; native fp32/fp64 arithmetic follows RES_native_contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

### Ports

`input` is a three-component opaque RGB semantic image; `sigma` is Float64 `[1]`; output has the same shape. Require sigma≥0. At sigma=0, explicitly copy without executing denoising stages. The initial RGB group must not be associated with alpha; callers must handle alpha explicitly.

### Parameters

The native profile parameters are not yet fixed by the family contract. This member requires explicit source color interpretation and a precisely specified color transform; Results do not carry source color metadata. Do not divide by 255 automatically; reject alpha, non-RGB, and AOV inputs.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

For sigma>0, use the native two-stage CBM3D algorithm with an explicitly fixed mathematical profile. Do not substitute NLM/TV or add video-frame or volumetric dimensions. Native arithmetic follows RES_native_contract and the explicitly selected FMT color-domain constraints. Any narrower mathematical domain must be stated by the native profile, not inherited from a comparison library input restriction.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Each output depends on the complete plane or RGB group. There is no fixed local halo. At sigma=0, copy input only and perform no algorithm-domain finite validation.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## References

[S20 · BM3D author project](../research-sources.md#s20)
