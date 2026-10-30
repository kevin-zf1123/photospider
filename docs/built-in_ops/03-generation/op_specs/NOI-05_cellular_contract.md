---
spec_schema_version: 1
id: NOI-05
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# NOI-05: Cellular and finite-site distance

F1/F2 are distance order statistics of a defined site set. A fixed 3x3 search is not an unconditional exact reference. Published station coordinates define the geometry. F2-F1 is not generally Euclidean distance to a Voronoi boundary.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-05A](NOI-05A_cellular2d_l2.md) | cellular2d l2 |
| [NOI-05B](NOI-05B_nearest_point_distances.md) | nearest point distances |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and the [random contract](NOI_random_contract.md).
