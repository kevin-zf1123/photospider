---
spec_schema_version: 1
id: GRD-14D
parent_id: GRD-14
function: threshold_sauvola
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.threshold_sauvola
---

# GRD-14D: Sauvola local threshold

Inherits the [family contract](GRD-14_threshold_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.threshold_sauvola` performs the transformation below.

## Interface

`feature: T[H,W]` -> `mask: UInt8[H,W]`.

Normalized feature in [0,1]; radius:Int64 0..4096 default 7; k:Float64 in [0,1] default 0.2; dynamic_range:Float64>0 default 0.5.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

`radius: Int64`, `k: Float64` and `dynamic_range: Float64` have defaults 7, 0.2 and 0.5. The normalized [H,W] feature is the only dynamic input.

## Mathematics

In the cropped square neighborhood, m=sum(x_q)/N and variance=sum((x_q-m)^2)/N. Set s=sqrt(variance), threshold=m*(1+k*(s/dynamic_range-1)). Return 1 iff x_p>=threshold. These are population moments accumulated in Float64.

### Current native algorithm/profile

Use full-image Float64 prefix grids for d=x-x(0,0) and d*d. Recover m=x(0,0)+mean(d) and v=max(0,mean(d*d)-mean(d)^2); v is the population variance. Apply the family refinement rule to the resulting threshold. Refinement computes the local mean first, then a second row-major pass over (x-m)^2. When k=0, use the local-mean decision before any division by dynamic_range; radius zero uses x>=x*(1-k).

## Behavior

The mathematical support is H(radius), cropped against the full image extent. ROI and tile edges retain the same full-image neighborhood. Output mask values are UInt8 0 or 1.

## Acceptance

Constant 0.5 with k=0.2 gives threshold 0.4 and mask 1. k=0 reduces to local-mean threshold with zero offset.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
