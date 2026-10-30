---
spec_schema_version: 1
id: PTH-09
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-09: Area filling

Fill_rule defaults to nonzero; evenodd is explicit. Coverage is true pixel area of the chosen filled geometry, not sample counts or smoothstep SDF. Flatten-based fill first publishes a polyline and retains that numerical boundary. Geometric approximation and final area rounding remain separate.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-09A](PTH-09A_fill_polygon_area.md) | fill polygon area |
| [PTH-09B](PTH-09B_fill_bezier_flatten.md) | fill bezier flatten |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).
