---
spec_schema_version: 1
id: PTH-05
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# PTH-05: Width profiles and binding

Width is finite nonnegative full diameter in geometry pixel units. Support either formal
attached width or independent input, with exactly one explicitly selected source. Three
explicit domains are whole-PathSet normalized arc, per-subpath normalized arc and
per-subpath arc pixels. Do not reinterpret the legacy ArcLength tag. Binding schemas
must represent source maps, closed seams and zero-length behavior.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-05A](PTH-05A_width_profile_linear.md) | width profile linear |
| [PTH-05B](PTH-05B_width_profile_pchip.md) | width profile pchip |
| [PTH-05C](PTH-05C_attach_linear_width.md) | attach linear width |

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
