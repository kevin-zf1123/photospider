---
spec_schema_version: 1
id: GEN-02
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# GEN-02: Coordinate grids

Canvas geometry is explicit; xy components and scalar axis sources are separate. Pixel centers define identity sampling coordinates, while normalized_edge uses W/H, not W-1/H-1.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-02A](GEN-02A_coordinates_xy.md) | coordinates xy |
| [GEN-02B](GEN-02B_coordinate_axis.md) | coordinate axis |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md).
