---
spec_schema_version: 1
id: RES-02A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: RES-02
function: nlm_plain
proposed_operation_keys:
- restoration.nlm_plain
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-02A: nlm_plain

Plain NLM.

Inherits the [RES-02 family contract](RES-02_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.nlm_plain`.

### Ports

`input,guide` → `output` with the input shape, layout and batch metadata. This member has no noise-sigma port. Both inputs have the same Float32/Float64 dtype and compatible logical sample shapes, as specified by the [family public API](RES-02_contract.md#interface).

### Parameters

Require patch_radius≥0, search_radius≥0, and h>0. Supply `guide_components` (G), `metric_scale`, `y_axis`, `x_axis` and `component_axis` explicitly. Parameter types and shape constraints are listed in the [family public API](RES-02_contract.md#interface).

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

Use the family patch rules. For p≠q, w=exp(-D/h²) in the input dtype. Distances, weights, numerator and denominator use native input precision; the self-weight of 1 guarantees a positive denominator.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Guide support is search+patch, at most H(s+r). Input reads only candidate centers in the search window. A weight that underflows to zero still depends on the guide data; input[p] is then not consumed. With search_radius=0, only the input center is read.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

A constant guide yields the candidate mean. search_radius=0 is identity. Check patch_radius=0 and single-pixel search against analytic weights. For member B, sigma=0 reduces to member A. Rescaling guide units requires corresponding rescaling of h and sigma.

## References

[S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)
