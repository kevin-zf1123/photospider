---
spec_schema_version: 1
id: PTH-11
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-11: Centerline and region distances

Unsigned centerline distance includes the original curve points; signed fill/stroke distance uses the actual exposed region boundary. Use (source subpath,segment,t) lexicographic closest ties. Inside is negative, outside positive, boundary +0. Internal overlap edges are excluded; min of SDFs is not general exact union distance.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-11A](PTH-11A_centerline_distance.md) | centerline distance |
| [PTH-11B](PTH-11B_fill_region_sdf.md) | fill region sdf |
| [PTH-11C](PTH-11C_stroke_region_sdf.md) | stroke region sdf |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).
