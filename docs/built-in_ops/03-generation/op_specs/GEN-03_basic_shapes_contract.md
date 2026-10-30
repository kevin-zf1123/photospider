---
spec_schema_version: 1
id: GEN-03
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# GEN-03: Shapes, coverage and distances

Generate coverage or geometry before explicit color/compositing. Coverage is pixel intersection area, not smoothed center distance. Rectangle/ellipse SDFs target actual Euclidean distance. Original curved geometry and explicit flattened geometry are different objects.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-03A](GEN-03A_rectangle_coverage.md) | rectangle coverage |
| [GEN-03B](GEN-03B_ellipse_coverage.md) | ellipse coverage |
| [GEN-03C](GEN-03C_polygon_coverage.md) | polygon coverage |
| [GEN-03D](GEN-03D_star_path.md) | star path |
| [GEN-03E](GEN-03E_rectangle_sdf.md) | rectangle sdf |
| [GEN-03F](GEN-03F_ellipse_sdf.md) | ellipse sdf |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md).
