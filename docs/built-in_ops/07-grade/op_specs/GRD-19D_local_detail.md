---
spec_schema_version: 1
id: GRD-19D
parent_id: GRD-19
function: local_detail
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.local_detail
---

# GRD-19D: Independent local detail strength

Inherits the [family contract](GRD-19_shadow_highlight_local_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.local_detail` performs the transformation below.

## Interface

`input: T[H,W,C]`, `detail_gain: Float64[1]` -> `values: T[H,W,C]`.

Relative linear RGB/Gray. Base parameters are as GRD-19A; detail_gain>=0, suggested 1.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Extract Y, z and base b at the Float64 boundaries in GRD-19C. Return x*exp2((detail_gain-1)*(z-b)). gain=1 copies; gain=0 removes log detail; gain>1 enhances it.

## Behavior

H(radius), with source colors at the output coordinate. Alpha/AOVs bypass independently.

## Acceptance

Constant Y is unchanged for every gain. Gain one is bit-preserving identity. Check a bright point with a nonzero neighborhood radius.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
