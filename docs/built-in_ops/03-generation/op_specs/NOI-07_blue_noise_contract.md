---
spec_schema_version: 1
id: NOI-07
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# NOI-07: Rank fields and threshold points

Generic rank lookup checks static structure only. Permutation, canonical little-endian Int64 payload and content identity are unchecked caller premises. Named blue-noise variants additionally require an approved asset,
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
Members declare Regional, Halo or complete-input from actual mathematical dependencies,
independently of static structure checks. GPU capability is member/backend-specific.
Same version/profile means bit-identical output; numerical error, geometry error and
topology guarantees are separate. Source associations are unchecked caller premises; producers preserve declared ownership and lifetime by construction.

## Acceptance and implementation boundary

Use member-specific independent fixtures plus actual public workflow execution. The
[oracle coverage](../oracle-coverage.md) identifies finite exact helpers, Measured
diagnostics and remaining unsupported cases. It cannot establish public schema support,
GPU admission or complete domain certification. The specified schemas and RNG mappings require implementation and validation before registration. Unresolved numerical solver or backend choices remain explicit member limitations.
