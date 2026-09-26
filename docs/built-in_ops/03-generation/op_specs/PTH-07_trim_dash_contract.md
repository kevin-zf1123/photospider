---
spec_schema_version: 1
id: PTH-07
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# PTH-07: Trim and dash

Operate on Core/Bezier paths; other authorities require explicit conversion. True
arc-length inversion is not uniform-t sampling. Preserve source-position width by
default. An explicit rebind is required to spread the whole profile across each output
fragment. Retain source parameter mappings even when control publication changes the new
path length.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-07A](PTH-07A_trim_path.md) | trim path |
| [PTH-07B](PTH-07B_dash_path.md) | dash path |

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
