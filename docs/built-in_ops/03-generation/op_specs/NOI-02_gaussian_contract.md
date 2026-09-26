---
spec_schema_version: 1
id: NOI-02
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# NOI-02: Gaussian samples

Use one cos-branch Box-Muller transform per addressed channel, with no cached neighbor
pairing. The mathematical target is the declared finite u/v grid, not a language-library
distribution. Finite tails and grid bias must be distinguished from ideal Gaussian
statistics.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-02A](NOI-02A_gaussian_box_muller.md) | gaussian box muller |

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
