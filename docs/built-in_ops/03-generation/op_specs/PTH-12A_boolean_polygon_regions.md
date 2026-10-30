---
spec_schema_version: 1
id: PTH-12A
parent_id: PTH-12
function: boolean_polygon_regions
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.boolean_polygon_regions_v1_strict
---

# PTH-12A: boolean polygon regions

Inherits [PTH-12](PTH-12_boolean_offset_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.boolean_polygon_regions_v1_strict`.

Two Polyline PathSets; Float64 CoreVerbs result and associated GeometryReport.

The associated Result uses `photospider.ops.geometry_report` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

### Parameters and domain

union/intersection/difference/xor; independent nonzero/evenodd fill rules; epsilon_publication_px>0 default1e-9; hard max_events; attribute_policy default reject_unmappable.

Capacity parameters (Int64): `max_events` (default 65536; the pairwise arrangement is bounded by it), `max_vertices` (4096), `max_contours` (256), `limb_capacity` (128). The arrangement is the exact rational polygon arrangement of PTH-12, followed by the RN64 publication and construction-time bound of PTH-12E.

## Mathematics

Lift input floats exactly, perform rational regularized Boolean, then publish using PTH-12E; equivalent to PTH-12C followed by E. Canonicalize outer shoelace positive in y-down, holes negative; remove shape-neutral degree2 collinear vertices, rotate each contour to lexicographically minimal full cycle, sort cycles. RN64 coordinates once, canonical +0. Displacement within the requested budget, preserved incidence and absence of new crossings or merges are unchecked caller premises. Accumulate displacement while constructing coordinates and report unverified topology; no publication scan. No snapping or silent tiny-hole removal.

## Behavior

Support class: complete input.

## Acceptance

Rectangle Boolean areas, contacts/holes, rational intersections, representable small features, idempotence and xor self-empty. Publication fixtures satisfy the unchecked rounding/topology premises; invalid geometry does not require a runtime diagnostic.
