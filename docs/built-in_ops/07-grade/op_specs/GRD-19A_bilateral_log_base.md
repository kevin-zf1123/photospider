---
spec_schema_version: 1
id: GRD-19A
parent_id: GRD-19
function: bilateral_log_base
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.bilateral_log_base
---

# GRD-19A: Bilateral base of log luminance

Inherits the [family contract](GRD-19_shadow_highlight_local_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.bilateral_log_base` performs the transformation below.

## Interface

`luminance: T[H,W]` -> `base: Float64[H,W]`.

Y>=0. Static radius:Int64 0..4096 default 3, sigma_space:Float64>0 default 2 pixels, sigma_range:Float64>0 default 1 stop, epsilon:Float64>0 default 2^-16.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

z_p=log2(max(Y_p,epsilon)). For q in the cropped square radius neighborhood, w_pq=exp(-distance(p,q)^2/(2*sigma_space^2)-(z_p-z_q)^2/(2*sigma_range^2)). base_p=sum(w_pq*z_q)/sum(w_pq). Use Float64 sums. Equal neighborhood z values select that z.

## Behavior

H(radius) on Y. Hidden straight colors participate when luminance is derived from an image. Tile boundaries retain full-image neighborhoods.

## Acceptance

Constant Y=0.25 gives base=-2. Radius zero gives log2(max(Y,epsilon)).

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
