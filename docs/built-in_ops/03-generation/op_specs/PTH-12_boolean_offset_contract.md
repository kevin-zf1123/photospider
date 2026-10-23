---
spec_schema_version: 1
id: PTH-12
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-12: Boolean regions, exact results and offsets

Exact-input and explicit-grid members have separate identities. Exact rational Result
and Float64 conversion with its construction-time displacement report are separate
outputs/operators. Float64 representability and preserved publication geometry are
unchecked caller premises; conversion does not validate completed geometry. Do not silently fall
back to grid, snap or remove small exact features. The selected production backend is the
in-repository exact rational `PolygonArrangement`; circular offsets use the separate
exact-radical `CircularArrangement`. No external geometry library is required.

## Frozen production algorithms and independent oracle

`plugins/ops/03-generation/polygon_arrangement.cpp` implements v1 polygon Boolean:

1. Lift each finite Float64 coordinate to its exact dyadic rational, or consume the canonical exact-polygon codec directly.
2. Enumerate edge pairs, split at exact rational intersections, and split collinear overlaps at their exact endpoints. No rounded intersection is used in a predicate.
3. Classify both sides of each atomic directed edge with exact winding/even-odd rules and symbolic infinitesimal side probes. Keep an edge only where the requested union/intersection/difference/xor changes region membership.
4. Trace the exposed oriented boundary. Remove shape-neutral collinear vertices only when their global boundary degree is two, preserving contacts shared by contours. Rotate each contour to its lexicographically minimal full cycle and sort cycles using exact comparisons.
5. Publish exact reduced rationals (C), or apply the separately specified RN64 publication stage (A/E). D/F use their explicit input quantization and final intersection snap; they are never an exact-input fallback.

The pairwise arrangement is deliberately bounded by `max_events`, `max_work`, output capacities, exact-arithmetic capacity and preadmitted scratch. It does not promise sweep-line asymptotics. All loops poll cancellation and exhaustion publishes no partial Result. The exact codec is base-2^32 little-endian with canonical reduced numerator/positive denominator and zeroed inactive storage.

`oracle/ops/generation/pth12/slab.py::verify_boolean` is an independent Python `Fraction` region oracle. It builds critical x coordinates from all input/output vertices and finite-edge intersections. Within each open slab, it compares complete affine upper/lower interval boundaries after fill-rule and set-operation evaluation. Edge order cannot change within a slab, so equality of these interval functions proves equality of the regularized two-dimensional regions, including holes, overlaps, self-crossings and point contacts. It does not certify canonical serialization or multiplicity by itself.

`pth12/geometry.py::boolean_regions` separately checks canonical directed contour output; its collinear removal preserves non-degree-two contact vertices. `pth12/grid.py` freezes the D/F quantization and publication reference. `pth12/fixtures.py` supplies rectangle, contact, hole, self-crossing and rational-intersection fixtures. These references are not linked into the production backend.

`circular_arrangement.cpp` supplies exact radical line/circle and circle/circle intersections, exposed-union classification and certified circular area/distance machinery for PTH-10, PTH-11C and PTH-12B. Finite directed refinement may fail when a predicate or final rounding cannot be certified within its limit; that failure is not an approximate answer.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-12A](PTH-12A_boolean_polygon_regions.md) | boolean polygon regions |
| [PTH-12B](PTH-12B_offset_euclidean_region.md) | offset euclidean region |
| [PTH-12C](PTH-12C_boolean_exact_result.md) | boolean exact result |
| [PTH-12D](PTH-12D_boolean_grid_result.md) | boolean grid result |
| [PTH-12E](PTH-12E_publish_exact_geometry.md) | publish exact geometry |
| [PTH-12F](PTH-12F_boolean_grid_float64.md) | boolean grid float64 |

## Shared execution and numerical rules

Inherit [GEN common](GEN_common_contract.md), and applicable
[random](NOI_random_contract.md) and [path geometry](PTH_geometry_contract.md) rules.
Members declare Regional, Halo or complete-input from actual mathematical dependencies,
independently of static structure checks. GPU capability is member/backend-specific.
Same version/profile means bit-identical output; numerical error, geometry error and
topology guarantees are separate. Source associations are unchecked caller premises; producers preserve declared ownership and lifetime by construction.

## Acceptance and implementation boundary

Use member-specific independent fixtures plus actual public workflow execution. The
[oracle coverage](../oracle-coverage.md) identifies finite exact helpers, Measured
diagnostics and remaining unsupported cases. It cannot establish public schema support,
GPU admission or complete domain certification. The CPU members and public authoring helpers are implemented. Finite arithmetic, refinement and capacity limits remain operational bounds; member-specific conservative topology proofs may reject unproven results. No GPU execution is registered for these members.
