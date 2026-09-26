---
spec_schema_version: 1
id: NOI-07
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# NOI-07: Rank fields and threshold points

Generic rank lookup validates shape, permutation, canonical little-endian Int64 bytes
and content identity. Named blue-noise variants additionally require an approved asset,
provenance/license and quality reports. A synthetic permutation fixture is not
production blue noise. Asset conversion creates a new identity and requires
requalification.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-07A](NOI-07A_blue_rank_tile.md) | blue rank tile |
| [NOI-07B](NOI-07B_blue_threshold_points.md) | blue threshold points |

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
