---
spec_schema_version: 1
id: GEN-07
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# GEN-07: Numeric mesh fields

Rectangular bilinear, triangle and regular tensor-product cubic Bernstein interpolation
are separate members. All components are generic numeric values. There is no
alpha-weighted mode, RGB metadata inference, curved-quad inverse mapping or automatic
patch stitching. Color/alpha workflows explicitly supply transformations and preserve
node rounding.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-07A](GEN-07A_bilinear_rectangle_gradient.md) | bilinear rectangle gradient |
| [GEN-07B](GEN-07B_triangle_mesh_gradient.md) | triangle mesh gradient |
| [GEN-07C](GEN-07C_bicubic_parameter_patch.md) | bicubic parameter patch |

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
