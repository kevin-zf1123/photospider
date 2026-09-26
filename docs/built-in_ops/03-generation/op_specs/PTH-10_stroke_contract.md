---
spec_schema_version: 1
id: PTH-10
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# PTH-10: Stroke coverage and outlines

Direct coverage targets the exact stroke union; polygonized outlines target a separate
approximated object. Width0 is empty, not device hairline. Resolve self-overlap
geometrically before coverage, not repeated source-over. Curved centerlines require
explicit approximation for these polyline members.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-10A](PTH-10A_stroke_constant_area.md) | stroke constant area |
| [PTH-10B](PTH-10B_stroke_variable_round_area.md) | stroke variable round area |
| [PTH-10C](PTH-10C_stroke_outline_flatten.md) | stroke outline flatten |

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
