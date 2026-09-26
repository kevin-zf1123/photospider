---
spec_schema_version: 1
id: NOI-01
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# NOI-01: Uniform noise

Address by integer global pixels and logical channel, never by rounded coordinate
tensors. The finite binary grid is explicit, not continuous uniform mathematics or a
perfectly flat finite-sample spectrum. Range remapping is a separate NUM workflow.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-01A](NOI-01A_uniform_philox.md) | uniform philox |

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
