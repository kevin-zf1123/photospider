---
spec_schema_version: 1
id: GEN-01
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# GEN-01: Constant fields and images

Raw bit filling and image construction with explicit color configuration are separate. A raw component count never implies RGB/alpha. Images use straight planar FMT representation. Each color consumer receives its own static `TensorDescription`; output Results contain structural schemas, samples and storage owners.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-01A](GEN-01A_constant_tensor.md) | constant tensor |
| [GEN-01B](GEN-01B_constant_image.md) | constant image |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md).
