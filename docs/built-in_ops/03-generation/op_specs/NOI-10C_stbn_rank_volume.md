---
spec_schema_version: 1
id: NOI-10C
parent_id: NOI-10
function: stbn_rank_volume
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
implementation_scope: generic_rank_resource
qualified_asset_status: not_provided
operation_keys:
  - noise.rank_volume_v1_strict
---

# NOI-10C: stbn rank volume

Inherits [NOI-10](NOI-10_temporal_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.rank_volume_v1_strict`.

rank[T,Th,Tw] Int64 and immutable resource descriptor; values[H,W].

### Parameters and domain

frame and offsets x/y/t Int64; canvas/dtype. Generic volume lookup checks static structure; valid payload and identity are unchecked premises; named STBN additionally requires approved provenance/license and spatial/temporal reports.

Descriptor parameters and the SDK descriptor helper follow NOI-07A, with N=T*Th*Tw<=2^40 and byte length 8N. Required: Int64 `height`, `width` and signed Int64 `frame`. Origins and offsets x/y/t default to 0; `dtype` defaults to `float64`. Address sums are formed without signed-64 overflow before the Euclidean modulo.

## Mathematics

complete-input volume is a permutation0..N-1, N=T*Th*Tw. Address frame+offset_t modulo T and global xy plus offsets modulo spatial shape; use NOI-07A midpoint mapping. This frame is a lookup coordinate, not the unsigned32 RNG frame field. SDK formats require explicit conversion and requalification.

## Behavior

Support class: Regional. The volume permutation and content identity are unchecked caller premises; the frame axis is periodic with period T.

## Acceptance

Negative frame, period T, valid rank permutations and explicit content identity; spatial-blue/time-white fixtures cannot certify STBN.
