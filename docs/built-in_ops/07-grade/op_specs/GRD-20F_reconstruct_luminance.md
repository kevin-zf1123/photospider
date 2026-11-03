---
spec_schema_version: 1
id: GRD-20F
parent_id: GRD-20
function: reconstruct_luminance
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.reconstruct_luminance
---

# GRD-20F: Set luminance by proportional RGB scaling

Inherits the [family contract](GRD-20_feature_curve_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.reconstruct_luminance` performs the transformation below.

## Interface

`input: T[S,C]`, `target: Float64[S,1]` -> `values: T[S,C]`.

Select relative linear RGB with an explicit basis. Target is relative linear Y.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Compute source Y from the Y row of the RGB-to-XYZ matrix in Float64. If target==Y, copy RGB. For Y!=0 return RGB*(target/Y). For Y=0 and nonzero target return the neutral triple (target,target,target).

### Current native algorithm/profile

The current native algorithm extracts binary64 Y with the explicit RGB matrix, checks exact target equality, then uses the proportional or zero-Y neutral branch stated above. Arithmetic RGB results convert once to T; copied components retain their original bits. The basis codec, selector parameters and native profile are specified by the [family](GRD-20_feature_curve_contract.md#current-parameters).

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

For neutral RGB=(0.25,0.25,0.25), target Y=0.5 gives (0.5,0.5,0.5) within tolerance. Nonzero-Y RGB ratios are retained.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
