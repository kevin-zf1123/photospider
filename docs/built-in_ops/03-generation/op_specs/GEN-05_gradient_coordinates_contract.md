---
spec_schema_version: 1
id: GEN-05
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# GEN-05: Gradient coordinates

Coordinates are dimensionless numeric fields; spread and color lookup are separate.
Pixel positions use global centers. Invalid coincident linear endpoints or nonpositive
radii fail rather than borrowing external SVG fallback behavior. Angular/repeat final
rounding keeps an upper endpoint rounded to 1. Two-circle validity is separate from
alpha.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-05A](GEN-05A_linear_gradient_coordinate.md) | linear gradient coordinate |
| [GEN-05B](GEN-05B_radial_gradient_coordinate.md) | radial gradient coordinate |
| [GEN-05C](GEN-05C_angular_gradient_coordinate.md) | angular gradient coordinate |
| [GEN-05D](GEN-05D_diamond_gradient_coordinate.md) | diamond gradient coordinate |
| [GEN-05E](GEN-05E_box_gradient_coordinate.md) | box gradient coordinate |
| [GEN-05F](GEN-05F_path_distance_coordinate.md) | path distance coordinate |
| [GEN-05G](GEN-05G_two_circle_radial_coordinate.md) | two circle radial coordinate |

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
