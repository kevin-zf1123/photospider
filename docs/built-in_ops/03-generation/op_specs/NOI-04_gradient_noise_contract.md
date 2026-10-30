---
spec_schema_version: 1
id: NOI-04
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# NOI-04: Gradient noise

Fixed Perlin 2002 permutation and seeded Philox gradient fields have distinct identities. Strict evaluates the complete mathematical polynomial; Java double-step identity is not promised. Coordinate units and spatial frequency mapping are explicit upstream operations.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-04A](NOI-04A_perlin2002_3d.md) | perlin2002 3d |
| [NOI-04B](NOI-04B_gradient2d_philox.md) | gradient2d philox |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and the [random contract](NOI_random_contract.md).
