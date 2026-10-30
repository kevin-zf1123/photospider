---
spec_schema_version: 1
id: PTH-10
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-10: Stroke coverage and outlines

Direct coverage targets the exact stroke union; polygonized outlines target a separate approximated object. Width0 is empty, not device hairline. Resolve self-overlap geometrically before coverage, not repeated source-over. Curved centerlines require explicit approximation for these polyline members.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-10A](PTH-10A_stroke_constant_area.md) | stroke constant area |
| [PTH-10B](PTH-10B_stroke_variable_round_area.md) | stroke variable round area |
| [PTH-10C](PTH-10C_stroke_outline_flatten.md) | stroke outline flatten |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).
