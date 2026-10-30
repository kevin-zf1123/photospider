---
spec_schema_version: 1
id: RES-04A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-04
function: bm3d_native
proposed_operation_keys: []
numeric_reference: F
oracle_scope: mathematical_reference_pending
---

# RES-04A: bm3d_native

Grayscale BM3D native algorithm contract.

Inherits the [RES-04 family contract](RES-04_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and this member define requirements for the native profile, whose detailed mathematical configuration remains unresolved; native fp32/fp64 arithmetic follows RES_native_contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

### Ports

`input` is a single-plane Float32/Float64 field; `sigma` is Float64 `[1]`; output has the same shape. Require sigma≥0. At sigma=0, explicitly copy without executing denoising stages.

### Parameters

The native profile parameters are not yet fixed by the family contract. This member accepts a grayscale single-plane input. Do not divide by 255 automatically; require one numerical plane; raw field metadata does not imply a color or alpha interpretation.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

For sigma>0, use the native two-stage BM3D algorithm with an explicitly fixed mathematical profile. Do not substitute NLM/TV or add video-frame or volumetric dimensions. Native arithmetic follows RES_native_contract for signed/HDR values. Parameter/control domains must be specified by the native mathematical profile, not inherited from a third-party library input restriction.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Each output depends on the complete plane or RGB group. There is no fixed local halo. At sigma=0, copy input only and perform no algorithm-domain finite validation.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## References

[S20 · BM3D author project](../research-sources.md#s20)
