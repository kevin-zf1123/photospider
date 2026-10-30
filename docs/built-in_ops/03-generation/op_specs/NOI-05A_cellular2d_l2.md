---
spec_schema_version: 1
id: NOI-05A
parent_id: NOI-05
function: cellular2d_l2
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.cellular2d_l2_v1_strict
  - noise.cellular2d_l2_v1_accelerated_apple_silicon
  - noise.cellular2d_l2_v1_accelerated_x86_64
---

# NOI-05A: cellular2d l2

Inherits [NOI-05](NOI-05_cellular_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.cellular2d_l2_v1_strict`, `noise.cellular2d_l2_v1_accelerated_apple_silicon`, `noise.cellular2d_l2_v1_accelerated_x86_64`.

coordinates[S...,2]; f1,f2[S...], nearest_cell[S...,2] Int64.

### Parameters and domain

dtype,seed/stream/frame; signed32 lattice; L2 metric; max_search_rings default 64.

Inputs are Float32/Float64 coordinate tuples. Outputs are independent `f1` and `f2` and an atomic `nearest_cell`. Parameters: String `dtype` (default `float64`); Int64 `seed` and `stream` (required); Int64 `frame` (default 0); Int64 `max_search_rings` (default 64; rings 0 through the limit inclusive). Square roots keep the final RN32/RN64 rounding in every profile. If the ring limit is exhausted without the F2 certificate, the operation fails with `OperationFailed` and the NotConverged domain code `0x4e4f490001`.

## Mathematics

Each domain7 cell uses its integer x/y, channel=0 and draw=0 with caller frame. Halfopen53 offsets use words 0/1 for x/y, leaving words 2/3 unused, and are published RN64 with explicit half-open-cell endpoint adjustment. Sort all supported sites by (distance_squared,iy,ix). Expand Chebyshev rings until the unvisited-cell closure lower bound is strictly greater than current F2; equality cannot discard a tie. All cells needed by the mathematical search must have representable signed32 addresses as an unchecked caller premise; the field is not truncated to repair an invalid address.

## Behavior

Support class: Regional.

## Acceptance

F1<=F2, brute-force crosscheck with exterior certificate, negative cells and distance ties; ring exhaustion fails.
