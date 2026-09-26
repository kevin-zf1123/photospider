---
spec_schema_version: 1
id: GEN-03
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# GEN-03: Shapes, coverage and distances

Generate coverage or geometry before explicit color/compositing. Coverage is pixel
intersection area, not smoothed center distance. Rectangle/ellipse SDFs target actual
Euclidean distance. Original curved geometry and explicit flattened geometry are
different objects.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-03A](GEN-03A_rectangle_coverage.md) | rectangle coverage |
| [GEN-03B](GEN-03B_ellipse_coverage.md) | ellipse coverage |
| [GEN-03C](GEN-03C_polygon_coverage.md) | polygon coverage |
| [GEN-03D](GEN-03D_star_path.md) | star path |
| [GEN-03E](GEN-03E_rectangle_sdf.md) | rectangle sdf |
| [GEN-03F](GEN-03F_ellipse_sdf.md) | ellipse sdf |

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
