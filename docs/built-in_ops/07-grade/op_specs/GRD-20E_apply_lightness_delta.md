---
spec_schema_version: 1
id: GRD-20E
parent_id: GRD-20
function: apply_lightness_delta
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.apply_lightness_delta
---

# GRD-20E: Apply a native lightness-increment field

Inherits the [family contract](GRD-20_feature_curve_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.apply_lightness_delta` performs the transformation below.

## Interface

`input: T[S,C]`, `delta: Float64[S,1]` -> `values: T[S,C]`.

Select CIELAB or OKLab. delta uses normalized CIE l or native OKLab L units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Add delta to the first coordinate and copy a,b. Values remain unclipped; delta=0 copies.

### Current native algorithm/profile

The selected normalized l or native L coordinate is widened to binary64, added to the matching dynamic delta, and converted once to T without clipping. Delta zero copies original bits; a,b and other components copy independently. The required consumer unit is `modulation_unit="delta_lightness"`. Other current parameters and the native numerical profile are specified by the [family](GRD-20_feature_curve_contract.md#current-parameters).

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

CIELAB l=0.5 with delta=0.1 yields l=0.6, a ten-unit change in CIE L*.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
