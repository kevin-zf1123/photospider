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
and Float64 conversion with its construction-time displacement report are separate
outputs/operators. Float64 representability and preserved publication geometry are
unchecked caller premises; conversion does not validate completed geometry. Do not silently fall
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
Members declare Regional, Halo or complete-input from actual mathematical dependencies,
independently of static structure checks. GPU capability is member/backend-specific.
Same version/profile means bit-identical output; numerical error, geometry error and
topology guarantees are separate. Source associations are unchecked caller premises; producers preserve declared ownership and lifetime by construction.

## Acceptance and implementation boundary

Use member-specific independent fixtures plus actual public workflow execution. The
[oracle coverage](../oracle-coverage.md) identifies finite exact helpers, Measured
diagnostics and remaining unsupported cases. It cannot establish public schema support,
GPU admission or complete domain certification. The specified schemas and RNG mappings require implementation and validation before registration. Unresolved numerical solver or backend choices remain explicit member limitations.
