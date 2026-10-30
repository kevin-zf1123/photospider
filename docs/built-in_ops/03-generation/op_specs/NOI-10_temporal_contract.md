---
spec_schema_version: 1
id: NOI-10
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# NOI-10: Continuous time and temporal resources

Discrete random frame, spatial advection and continuous time are different variables. Continuous fields do not reseed each frame. Time is seconds and velocity coordinate units/second. Periodic rank-volume lookup does not automatically qualify as STBN.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-10A](NOI-10A_advected_gradient2d.md) | advected gradient2d |
| [NOI-10B](NOI-10B_temporal_perlin3d.md) | temporal perlin3d |
| [NOI-10C](NOI-10C_stbn_rank_volume.md) | stbn rank volume |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and the [random contract](NOI_random_contract.md).
