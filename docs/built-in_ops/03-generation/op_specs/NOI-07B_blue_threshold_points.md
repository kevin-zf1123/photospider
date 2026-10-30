---
spec_schema_version: 1
id: NOI-07B
parent_id: NOI-07
function: blue_threshold_points
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
implementation_scope: generic_rank_resource
qualified_asset_status: not_provided
operation_keys:
  - noise.rank_threshold_points_v1_strict
---

# NOI-07B: blue threshold points

Inherits [NOI-07](NOI-07_blue_noise_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.rank_threshold_points_v1_strict`.

Rank/resource as NOI-07A; [DynamicPoints v1](GEN-08_point_distributions_contract.md) Result, at most one point per canvas pixel.

### Parameters and domain

canvas,offsets,threshold_count K=0..N; nonnegative static hard output capacity max_count, with physical P=max(1,max_count). Zero capacity permits an empty result only; generating a selected point beyond the logical capacity fails operationally.

Descriptor parameters follow NOI-07A. Required Int64 parameters: `height`, `width`, `threshold_count`. Origins and offsets default to 0; Int64 `max_count` defaults to 1048576. There is no `dtype` parameter; output `points` is the canonical DynamicPoints v1 bundle (positions, ordinal IDs, count, zero padding). Selecting more points than `max_count` fails with ResourceExhausted/CapacityLimit and publishes no prefix. A zero threshold reads no rank payload.

## Mathematics

Select rank<K, publish global pixel centers RN64 in y/x scan order with ordinal IDs. One complete resource period yields exactly K points; cropped/repeated coverage changes count. K=0 is empty. No continuous Poisson-disk or minimum-distance claim.

## Behavior

Support class: complete input. The output covers the selected canvas support; resource validity is not scanned. `max_count` is a hard capacity, not the normal count target of GEN-08C/D.

## Acceptance

Full-period counts, nested thresholds, crop/origin invariance and capacity rejection.
