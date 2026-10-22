---
spec_schema_version: 1
id: NOI-04
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# NOI-04: Gradient noise

Fixed Perlin 2002 permutation and seeded Philox gradient fields have distinct
identities. Strict evaluates the complete mathematical polynomial; Java double-step
identity is not promised. Coordinate units and spatial frequency mapping are explicit
upstream operations.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-04A](NOI-04A_perlin2002_3d.md) | perlin2002 3d |
| [NOI-04B](NOI-04B_gradient2d_philox.md) | gradient2d philox |

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
GPU admission or complete domain certification. The listed members are implemented through fixed invocation ABI 8 and the public generation SDK. Unresolved numerical solver or backend choices remain explicit member limitations.
