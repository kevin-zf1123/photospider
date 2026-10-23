---
spec_schema_version: 1
id: PTH-09
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-09: Area filling

Fill_rule defaults to nonzero; evenodd is explicit. Coverage is true pixel area of the
chosen filled geometry, not sample counts or smoothstep SDF. Flatten-based fill first
publishes a polyline and retains that numerical boundary. Geometric approximation and
final area rounding remain separate.

## Current implementation scope

PTH-09A is implemented by `plugins/ops/03-generation/polygon.cpp`; GEN-03C
exposes that fill through the public generation authoring helper. PTH-09B is
implemented by `generation::fill_bezier_path`, composing PTH-04D with PTH-09A
and retaining the separate geometry-error report.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-09A](PTH-09A_fill_polygon_area.md) | fill polygon area |
| [PTH-09B](PTH-09B_fill_bezier_flatten.md) | fill bezier flatten |

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
