---
spec_schema_version: 1
id: PTH-03
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-03: Arc length and tables

Length is RN64 of true length. epsilon controls enclosure/table geometry, not strict numeric tolerance. Matching high-precision estimates do not establish correct rounding. Table layout and values are invariant under output selection, partitioning and scheduling.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-03A](PTH-03A_bezier_arc_length.md) | bezier arc length |
| [PTH-03B](PTH-03B_primitive_arc_length.md) | primitive arc length |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).
