---
spec_schema_version: 1
id: GEN-08
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# GEN-08: Point distributions

Publish complete DynamicPoints results with Float64 positions, ordinal IDs and valid
ID-to-row associations. Minimum-distance predicates operate on published points.
Grid/jitter have fixed nx*ny counts and hard capacity; rejection/FIFO have normal
max_count stop targets plus separate hard budgets. No maximal packing/density promise.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-08A](GEN-08A_grid_points.md) | grid points |
| [GEN-08B](GEN-08B_jittered_grid_points.md) | jittered grid points |
| [GEN-08C](GEN-08C_poisson_rejection_points.md) | poisson rejection points |
| [GEN-08D](GEN-08D_bridson_points.md) | bridson points |

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
