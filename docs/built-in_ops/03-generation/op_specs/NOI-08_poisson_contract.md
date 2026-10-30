---
spec_schema_version: 1
id: NOI-08
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# NOI-08: Poisson counts and electron shot

The inverse-CDF sequence and electron-unit workflow are separate. Expected electrons are nonnegative as an unchecked caller premise. Read noise, gain, black level, full well, clipping and quantization are explicit later operations. Integer branch decisions do not receive accelerated numerical tolerance.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-08A](NOI-08A_poisson_icdf.md) | poisson icdf |
| [NOI-08B](NOI-08B_shot_electrons.md) | shot electrons |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and the [random contract](NOI_random_contract.md).
