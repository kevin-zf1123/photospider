---
spec_schema_version: 1
id: NOI-02
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# NOI-02: Gaussian samples

Use one cos-branch Box-Muller transform per addressed channel, with no cached neighbor pairing. The mathematical target is the declared finite u/v grid, not a language-library distribution. Finite tails and grid bias must be distinguished from ideal Gaussian statistics.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-02A](NOI-02A_gaussian_box_muller.md) | gaussian box muller |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and the [random contract](NOI_random_contract.md).
