---
spec_schema_version: 1
id: PTH-12
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-12: Boolean regions, exact results and offsets

Exact-input and explicit-grid members have separate identities. The exact rational Result and the Float64 conversion with its construction-time displacement report are separate outputs and operators. Float64 representability and preserved publication geometry are unchecked caller premises; conversion does not validate completed geometry. There is no silent fallback to grid geometry, snapping or removal of small exact features. Polygon Boolean uses an exact rational arrangement; circular offsets use an exact-radical circle arrangement.

## Algorithm

The v1 polygon Boolean is:

1. Lift each finite Float64 coordinate to its exact dyadic rational, or consume the canonical exact-polygon codec directly.
2. Enumerate edge pairs, split at exact rational intersections, and split collinear overlaps at their exact endpoints. No rounded intersection is used in a predicate.
3. Classify both sides of each atomic directed edge with exact winding or even-odd rules and symbolic infinitesimal side probes. Keep an edge only where the requested union/intersection/difference/xor changes region membership.
4. Trace the exposed oriented boundary. Remove shape-neutral collinear vertices only when their global boundary degree is two, preserving contacts shared by contours. Rotate each contour to its lexicographically minimal full cycle and sort cycles using exact comparisons.
5. Publish exact reduced rationals (PTH-12C), or apply the separately specified RN64 publication stage (PTH-12A/E). PTH-12D/F use their explicit input quantization and final intersection snap and are never an exact-input fallback.

The exact codec is base-2^32 little-endian with a canonical reduced numerator, a positive denominator and zeroed inactive storage. The exact-radical circle arrangement supplies line/circle and circle/circle intersections, exposed-union classification and certified circular area and distance for PTH-10, PTH-11C and PTH-12B. When a predicate or final rounding cannot be certified, the operation fails; it does not return an approximate answer.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-12A](PTH-12A_boolean_polygon_regions.md) | boolean polygon regions |
| [PTH-12B](PTH-12B_offset_euclidean_region.md) | offset euclidean region |
| [PTH-12C](PTH-12C_boolean_exact_result.md) | boolean exact result |
| [PTH-12D](PTH-12D_boolean_grid_result.md) | boolean grid result |
| [PTH-12E](PTH-12E_publish_exact_geometry.md) | publish exact geometry |
| [PTH-12F](PTH-12F_boolean_grid_float64.md) | boolean grid float64 |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).
