---
spec_schema_version: 1
id: GRD-20C
parent_id: GRD-20
function: apply_hue_delta
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.apply_hue_delta
---

# GRD-20C: Apply a hue-increment field

Inherits the [family contract](GRD-20_feature_curve_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.apply_hue_delta` performs the transformation below.

## Interface

`input: T[S,C]`, `delta: Float64[S,1]` -> `values: T[S,C]`.

Select HSL, HSV, CIELCh or OKLCh. Required hue_unit=radian|pi_multiple for both source hue and delta. HSL/HSV saturation is bounded; polar chroma is nonnegative.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

When source saturation/chroma is zero, copy h; otherwise h_out=h+delta. All other components copy independently. Stored hue retains winding.

### Current native algorithm/profile

The source hue and dynamic delta are evaluated in binary64 in their shared declared unit. Zero delta or zero original saturation/chroma selects the stored hue bits; otherwise one addition retains the hue winding. The required consumer unit is `modulation_unit="delta_h"`. Other current parameters and the native numerical profile are specified by the [family](GRD-20_feature_curve_contract.md#current-parameters).

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

A pi_multiple hue increment 0.5 rotates a chromatic pixel by a quarter-turn. Gray hue remains unchanged.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
