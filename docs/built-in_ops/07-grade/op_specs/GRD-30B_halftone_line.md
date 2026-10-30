---
spec_schema_version: 1
id: GRD-30B
parent_id: GRD-30
function: halftone_line
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.halftone_line
---

# GRD-30B: Line-screen halftone

Inherits the [family contract](GRD-30_halftone_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.halftone_line` performs the transformation below.

## Interface

`coverage: T[H,W]` -> `ink: T[H,W]`.

The domain, basis, origin, samples_per_axis and edge parameters are those of GRD-30A.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Use the same subpixel cell coordinates. A sample is ink iff abs(u-0.5)<coverage/2. Average S^2 binary decisions. Coverage 0/1 selects exact 0/1.

## Behavior

E support. Cell orientation and full-image coordinates determine the lines independently of tile requests.

## Acceptance

Zero/full coverage returns 0/1. Swapping/rotating the cell basis changes the declared screen orientation.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
