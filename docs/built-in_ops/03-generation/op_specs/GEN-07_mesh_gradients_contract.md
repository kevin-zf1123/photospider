---
spec_schema_version: 1
id: GEN-07
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# GEN-07: Numeric mesh fields

Rectangular bilinear, triangle and regular tensor-product cubic Bernstein interpolation are separate members. All components are generic numeric values. There is no alpha-weighted mode, RGB metadata inference, curved-quad inverse mapping or automatic patch stitching. Color/alpha workflows explicitly supply transformations and preserve node rounding.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-07A](GEN-07A_bilinear_rectangle_gradient.md) | bilinear rectangle gradient |
| [GEN-07B](GEN-07B_triangle_mesh_gradient.md) | triangle mesh gradient |
| [GEN-07C](GEN-07C_bicubic_parameter_patch.md) | bicubic parameter patch |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md).
