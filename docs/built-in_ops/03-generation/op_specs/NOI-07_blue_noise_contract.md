---
spec_schema_version: 1
id: NOI-07
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
implementation_scope: generic_rank_resources
---

# NOI-07: Rank fields and threshold points

Generic rank lookup checks static structure only. Permutation, canonical little-endian Int64 payload and content identity are unchecked caller premises. Named blue-noise variants additionally require an approved asset, provenance/license and quality reports. A synthetic permutation fixture is not production blue noise. Asset conversion creates a new identity and requires requalification.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-07A](NOI-07A_blue_rank_tile.md) | blue rank tile |
| [NOI-07B](NOI-07B_blue_threshold_points.md) | blue threshold points |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and the [random contract](NOI_random_contract.md).
