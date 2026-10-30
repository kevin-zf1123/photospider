---
spec_schema_version: 1
id: NOI-03
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# NOI-03: Correlated noise

The numeric correlation filter and the white-source workflow are separate. Kernel orientation is correlation, with no implicit flip. Sum and L2 normalization express different guarantees. Repeated boundary samples share a random variable. Final arithmetic error cannot be allocated independently to every tap.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-03A](NOI-03A_correlate_kernel.md) | correlate kernel |
| [NOI-03B](NOI-03B_correlated_gaussian.md) | correlated gaussian |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and the [random contract](NOI_random_contract.md).
