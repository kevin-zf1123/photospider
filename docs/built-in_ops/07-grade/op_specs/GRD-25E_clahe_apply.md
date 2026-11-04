---
spec_schema_version: 1
id: GRD-25E
parent_id: GRD-25
function: clahe_apply
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.clahe_apply
---

# GRD-25E: Apply local CLAHE maps

Inherits the [family contract](GRD-25_histogram_equalize_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.clahe_apply` performs the transformation below.

## Interface

`feature: T[H,W]`, `maps: Float64[Ty,Tx,B]`, `identity: UInt8[Ty,Tx]` -> `values: T[H,W]`.

Static `cell_width:Int64=64`, `cell_height:Int64=64`, both >=1, and `edge:String=clamp|reject`, default clamp, match GRD-25D. B is inferred from the final maps extent, not supplied as a parameter. `max_work:Int64` defaults to 9223372036854775807. Shape extents obey GRD-25D Tx/Ty formulas; maps are nondecreasing in [0,1], flags are 0/1.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Use global pixel centers. Bracket each coordinate by actual cropped cell centers and clamp beyond the first/last center. In each of up to four cells use u when identity=1, otherwise its step-map value at bin(u). Bilinearly blend these values. If all contributing cells are identity, copy u.

## Behavior

Local feature plus neighboring cell flags and map entries. Complete neighboring maps are an admitted support superset. An empty cell contributes its identity transform to the spatial blend.

## Acceptance

All-identity maps preserve in-domain input. At an exact cell center use that cell alone. Check continuity of interpolation across cell-center intervals and physical tile boundaries.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
