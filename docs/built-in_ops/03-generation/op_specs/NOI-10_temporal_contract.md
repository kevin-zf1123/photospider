---
spec_schema_version: 1
id: NOI-10
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# NOI-10: Continuous time and temporal resources

Discrete random frame, spatial advection and continuous time are different variables.
Continuous fields do not reseed each frame. Time is seconds and velocity coordinate
units/second. Periodic rank-volume lookup does not automatically qualify as STBN.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-10A](NOI-10A_advected_gradient2d.md) | advected gradient2d |
| [NOI-10B](NOI-10B_temporal_perlin3d.md) | temporal perlin3d |
| [NOI-10C](NOI-10C_stbn_rank_volume.md) | stbn rank volume |

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
