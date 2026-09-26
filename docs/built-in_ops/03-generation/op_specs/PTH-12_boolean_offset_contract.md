---
spec_schema_version: 1
id: PTH-12
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# PTH-12: Boolean regions, exact results and offsets

Exact-input and explicit-grid members have separate identities. Exact rational Result
and validated Float64 publication are separate outputs/operators. Do not silently fall
back to grid, snap or remove small exact features. Candidate backend validation must
cover geometry, canonical output, codecs, allocation, cancellation, lifetime and
publication. No library is preselected. Circular offset may require nonrational
constructions.

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
Members declare Regional, Halo or Whole from actual mathematical dependencies,
independently of complete control validation. GPU capability is member/backend-specific.
Same version/profile means bit-identical output; numerical error, geometry error and
topology guarantees are separate. Public Result data requires validated source
associations and lifetime.

## Acceptance and implementation boundary

Use member-specific independent fixtures plus actual public workflow execution. The
[oracle coverage](../oracle-coverage.md) identifies finite exact helpers, Measured
diagnostics and remaining unsupported cases. It cannot establish public schema support,
GPU admission or complete domain certification. Concrete schema, RNG mapping and
backend/solver details identified by the member must be specified and validated before
registering that member.
