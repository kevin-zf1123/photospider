---
spec_schema_version: 1
id: PTH-06
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# PTH-06: Path and outline transforms

Transforming a centerline before fixed-pixel stroking differs from transforming an
existing stroke outline. Only affine 2x3 maps belong to these members; perspective needs
a separate infinity/clipping contract. Preserve defined attribute associations or
reject/drop explicitly.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-06A](PTH-06A_transform_centerline.md) | transform centerline |
| [PTH-06B](PTH-06B_transform_stroke_outline.md) | transform stroke outline |

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
