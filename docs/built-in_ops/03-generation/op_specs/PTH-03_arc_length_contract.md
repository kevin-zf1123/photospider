---
spec_schema_version: 1
id: PTH-03
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-03: Arc length and tables

Length is RN64 of true length. epsilon controls enclosure/table geometry, not strict
numeric tolerance. Matching high-precision estimates do not establish correct rounding.
Table layout and values are invariant under output selection, partitioning and
scheduling. Formal ArcLengthTable associations remain to be implemented.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-03A](PTH-03A_bezier_arc_length.md) | bezier arc length |
| [PTH-03B](PTH-03B_primitive_arc_length.md) | primitive arc length |

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
