---
spec_schema_version: 1
id: RES-04A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: RES-04
function: bm3d_native
proposed_operation_keys:
- restoration.bm3d_native
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-04A: bm3d_native

Grayscale BM3D native algorithm contract.

Inherits the [RES-04 family contract](RES-04_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The fixed profile is `haar_opponent_v1`, defined by the family contract. Native fp32/fp64 arithmetic follows RES_native_contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.bm3d_native`. The C++ SDK helper `ps::restoration::bm3d_native` appends one node transactionally.

### Ports

Inputs, in order: `input` is one nonempty rank-two Float32/Float64 numerical plane with no batch axes; `sigma` is one Float64 tensor of shape `[1]` with no batch axes. The `output` port contains a `photospider.tensor` v1 Result with one tensor named `samples`, retaining the input shape, dtype and layout. Sigma is nonnegative in input sample units. At sigma=0, copy input bits without denoising.

### Parameters

Supply every algorithm parameter in the [family parameter table](RES-04_contract.md#shared-parameters). The SDK accepts a `Bm3dOptions` value and explicit input/sigma references. The two spatial axes cover the rank-two plane. Input values retain their numerical units; raw field metadata does not introduce a color or alpha interpretation.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

For sigma>0, execute the family hard-threshold stage, then the pilot-guided Wiener stage. Patch distances use the single input plane or its basic estimate. Both matching thresholds are in squared input units. Signed and HDR samples retain their units throughout; output is unbounded native Float32/Float64.
Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Each output depends on the complete plane or RGB group. There is no fixed local halo. At sigma=0, copy input only and perform no algorithm-domain finite validation.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Apply the [family acceptance cases](RES-04_contract.md#behavior-and-acceptance). Check both spatial-axis orders and preserve signed/HDR units.

## References

[S20 · BM3D author project](../research-sources.md#s20)
