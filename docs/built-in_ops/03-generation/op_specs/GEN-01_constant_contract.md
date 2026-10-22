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
GPU admission or complete domain certification. The listed members are implemented through fixed invocation ABI 8 and the public generation SDK. Unresolved numerical solver or backend choices remain explicit member limitations.
