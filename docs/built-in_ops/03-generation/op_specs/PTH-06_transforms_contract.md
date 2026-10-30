---
spec_schema_version: 1
id: PTH-06
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-06: Path and outline transforms

Transforming a centerline before fixed-pixel stroking differs from transforming an existing stroke outline. Only affine 2x3 maps belong to these members; perspective needs a separate infinity/clipping contract. Preserve defined attribute associations or reject/drop explicitly.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-06A](PTH-06A_transform_centerline.md) | transform centerline |
| [PTH-06B](PTH-06B_transform_stroke_outline.md) | transform stroke outline |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).
