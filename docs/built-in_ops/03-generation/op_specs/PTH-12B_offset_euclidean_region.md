---
spec_schema_version: 1
id: PTH-12B
parent_id: PTH-12
function: offset_euclidean_region
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.offset_euclidean_region_v1_strict
---

# PTH-12B: offset euclidean region

Inherits [PTH-12](PTH-12_boolean_offset_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.offset_euclidean_region_v1_strict`.

Polyline filled region and finite distance[1] in px; CoreVerbs offset outline and GeometryReport.

### Parameters and domain

fill_rule; epsilon_outline_px>0 default0.05,epsilon_publication_px,max_events/max_segments; Euclidean round offset, no miter/bevel aliases.

Dilation unions the Euclidean boundary band with the regularized source region; erosion subtracts it and may remove the region entirely. `epsilon_outline_px`, stage-bound accumulation, deterministic subdivision and `topology_policy` follow PTH-10C. The global outline bound includes straight offset edges and every retained split vertex; publication error alone can make a requested outline impossible even when arc approximation is zero, and such a request fails before output publication. Nonzero circular offsets are not replaced by rational-only polygon joins.

## Mathematics

Positive d is Minkowski sum with closed disk(d); negative d is disk erosion; zero yields canonical region. Compute actual exposed offset boundary, then explicit bounded arc polygonization/publication. Negative offset may legitimately grow holes or remove components. Preserve distinction between true target changes and approximation changes; topology policy follows explicit outline rules.

## Behavior

Support class: complete input. Line/circle constructions and an offset arrangement are required; a rational polygon construction alone does not provide circular offsets, and grid offsets have distinct semantics.

## Acceptance

Disk radius change, rectangle rounded corners, erosion beyond half short side empty, zero identity and certified neck separation. Eroding the unit square by the Float64 value `0.1` with `epsilon_outline_px=1e-20` must fail when the exact-to-Float64 vertex displacement exceeds that bound.
