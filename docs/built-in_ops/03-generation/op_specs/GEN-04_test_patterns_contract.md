---
spec_schema_version: 1
id: GEN-04
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# GEN-04: Diagnostic patterns

Patterns are point-sampled unless their member explicitly states otherwise. Frequency,
phase and diagnostic aliasing are observable. RGB bars are eight full-range linear
diagnostic colors, not SMPTE/EBU encoded signals.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-04A](GEN-04A_checker_pattern.md) | checker pattern |
| [GEN-04B](GEN-04B_grid_pattern.md) | grid pattern |
| [GEN-04C](GEN-04C_ramp_pattern.md) | ramp pattern |
| [GEN-04D](GEN-04D_impulse_pattern.md) | impulse pattern |
| [GEN-04E](GEN-04E_zone_plate.md) | zone plate |
| [GEN-04F](GEN-04F_siemens_star.md) | siemens star |
| [GEN-04G](GEN-04G_linear_rgb_bars.md) | linear rgb bars |

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
