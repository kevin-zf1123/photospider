---
spec_schema_version: 1
id: PTH-11
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# PTH-11: Centerline and region distances

Unsigned centerline distance includes the original curve points; signed fill/stroke
distance uses the actual exposed region boundary. Use (source subpath,segment,t)
lexicographic closest ties. Inside is negative, outside positive, boundary +0. Internal
overlap edges are excluded; min of SDFs is not general exact union distance.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-11A](PTH-11A_centerline_distance.md) | centerline distance |
| [PTH-11B](PTH-11B_fill_region_sdf.md) | fill region sdf |
| [PTH-11C](PTH-11C_stroke_region_sdf.md) | stroke region sdf |

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
