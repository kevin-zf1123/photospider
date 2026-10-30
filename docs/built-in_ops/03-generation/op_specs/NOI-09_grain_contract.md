---
spec_schema_version: 1
id: NOI-09
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# NOI-09: Multiplicative speckle and artistic grain

Integer-looks speckle and linear-RGB grain have different models. Grain is not a calibrated camera or film model. Its explicit normalization and kernel determine noise scale; strength is a multiplier. Alpha is copied, including signed zero, and hidden RGB is processed. Other color domains require separate operators.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-09A](NOI-09A_speckle_integer_looks.md) | speckle integer looks |
| [NOI-09B](NOI-09B_artistic_linear_grain.md) | artistic linear grain |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and the [random contract](NOI_random_contract.md).
