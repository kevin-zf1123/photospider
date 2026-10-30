---
spec_schema_version: 1
id: NOI-01
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# NOI-01: Uniform noise

Address by integer global pixels and logical channel, never by rounded coordinate tensors. The finite binary grid is explicit, not continuous uniform mathematics or a perfectly flat finite-sample spectrum. Range remapping is a separate NUM workflow.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-01A](NOI-01A_uniform_philox.md) | uniform philox |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and the [random contract](NOI_random_contract.md).
