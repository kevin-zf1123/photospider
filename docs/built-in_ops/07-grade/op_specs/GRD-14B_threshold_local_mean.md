---
spec_schema_version: 1
id: GRD-14B
parent_id: GRD-14
function: threshold_local_mean
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.threshold_local_mean
---

# GRD-14B: Local-mean threshold

Inherits the [family contract](GRD-14_threshold_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.threshold_local_mean` performs the transformation below.

## Interface

`feature: T[H,W]`, `offset: Float64[1]` -> `mask: UInt8[H,W]`.

Static radius:Int64 0..4096, default 3. Suggested offset=0 in feature units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

`radius: Int64` defaults to 3. The image and Float64[1] offset remain required dynamic inputs; this member accepts one [H,W] image.

## Mathematics

For the cropped square radius neighborhood N_p, m=sum(x_q)/|N_p|. Return 1 iff x_p>=m-offset. Accumulation uses Float64 and the common order.

### Current native algorithm/profile

Use a full-image Float64 prefix grid for d=x-x(0,0). A cropped rectangle gives mean=x(0,0)+sum(d)/N. Candidate comparisons may be formed in scaled-sum coordinates. Apply the family numerical refinement rule near a decision boundary, preserving the Float64 mean and subsequent mean-offset subtraction. Radius zero directly compares x against x-offset.

## Behavior

The mathematical support is H(radius), cropped against the full image extent. ROI and tile edges retain the same full-image neighborhood. Output mask values are UInt8 0 or 1.

## Acceptance

A constant feature with zero offset yields all ones. Radius zero compares x against x-offset.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
