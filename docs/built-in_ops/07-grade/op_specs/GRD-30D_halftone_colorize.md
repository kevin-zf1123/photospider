---
spec_schema_version: 1
id: GRD-30D
parent_id: GRD-30
function: halftone_colorize
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.halftone_colorize
---

# GRD-30D: Artistic color composition of ink masks

Inherits the [family contract](GRD-30_halftone_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.halftone_colorize` performs the transformation below.

## Interface

`ink: T[H,W,K]`, `colors: Float64[K,3]`, `background: Float64[3]` -> `values: T[H,W,3]`.

1<=K<=64; ink in [0,1]. Required destination linear RGB description. Row order defines the artistic layer order from back to front; colors/background use that RGB basis.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Start v=background. For k=0..K-1 set v=(1-ink_k)*v+ink_k*colors[k]. Return v converted to T. This is ordered artistic coverage blending on an opaque background.

## Behavior

E on all K ink masks and colors/background. The output is RGB; any image alpha is assembled separately. The model specifies artistic blending rather than physical paper, ink absorption or overprint.

## Acceptance

All-zero masks return the background. A final full-coverage layer returns its ink color. With black on white and coverage 0.25 output neutral 0.75.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
