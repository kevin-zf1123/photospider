---
spec_schema_version: 1
id: NOI-03
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# NOI-03: Correlated noise

The numeric correlation filter and the white-source workflow are separate. Kernel
orientation is correlation, with no implicit flip. Sum and L2 normalization express
different guarantees. Repeated boundary samples share a random variable. Final
arithmetic error cannot be allocated independently to every tap.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-03A](NOI-03A_correlate_kernel.md) | correlate kernel |
| [NOI-03B](NOI-03B_correlated_gaussian.md) | correlated gaussian |

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
