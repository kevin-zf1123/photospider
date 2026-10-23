---
spec_schema_version: 1
id: PTH-05
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-05: Width profiles and binding

Width is finite nonnegative full diameter in geometry pixel units. Select exactly one explicit source: a separate path_width Result paired with the PathSet by the workflow, or an independent width-evaluation input. Geometry Results carry no implicit width field. Three
explicit domains are whole-PathSet normalized arc, per-subpath normalized arc and
per-subpath arc pixels. The generic ArcLength label does not distinguish these three domains. Binding schemas
must represent source maps, closed seams and zero-length behavior.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-05A](PTH-05A_width_profile_linear.md) | width profile linear |
| [PTH-05B](PTH-05B_width_profile_pchip.md) | width profile pchip |
| [PTH-05C](PTH-05C_attach_linear_width.md) | pair the original path with a separate Linear width Result |

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
