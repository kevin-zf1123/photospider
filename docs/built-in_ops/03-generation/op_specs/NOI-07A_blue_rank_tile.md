---
spec_schema_version: 1
id: NOI-07A
parent_id: NOI-07
function: blue_rank_tile
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
implementation_scope: generic_rank_resource
qualified_asset_status: not_provided
operation_keys:
  - noise.rank_tile_v1_strict
---

# NOI-07A: blue rank tile

Inherits [NOI-07](NOI-07_blue_noise_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.rank_tile_v1_strict`.

rank[Th,Tw] Int64 and immutable resource descriptor; values[H,W].

### Parameters and domain

canvas,dtype; offset_x/y Int64 default0; N=Th*Tw<=2^40; no implicit random shift.

Required parameters: Int64 `height`, `width`; String `resource_sha256` (64 lowercase hex digits); Int64 `resource_byte_length` (equal to 8N). String `resource_encoding` defaults to and only accepts `little_endian_int64`. The tile shape comes from the rank input schema. Origins and offsets default to 0; `dtype` defaults to `float64`. The descriptor is authored by the public SDK helper `Result<Parameters> ps::generation::rank_resource_parameters(const ResourceIdentity& identity)` (declared in `photospider/ops/generation.hpp`) and bound by the caller to the immutable input Result. The payload permutation and content identity are not scanned.

## Mathematics

The rank payload is a permutation 0..N-1 as an unchecked caller premise. Euclidean-periodic global addressing yields r; return RN_T((r+1/2)/N), explicitly nextDown(1) if rounded1 and nextUp(0) if rounded0. This rank mapping is separate from periodic gradient coordinates. Generic lookup requires structure/content identity; named blue variants additionally require approved provenance/license/quality.

## Behavior

Support class: Regional. Only the demanded periodic support of the rank tensor contributes. The permutation and content identity are unchecked caller premises.

## Acceptance

Permutation, negative offsets, valid immutable content binding, midpoint grid and no blue-quality inference from synthetic fixtures.
