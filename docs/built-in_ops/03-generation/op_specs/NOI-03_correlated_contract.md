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
