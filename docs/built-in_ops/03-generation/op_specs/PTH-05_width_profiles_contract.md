---
spec_schema_version: 1
id: PTH-05
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-05: Width profiles and binding

Width is finite nonnegative full diameter in geometry pixel units. Select exactly one explicit source: a separate path_width Result paired with the PathSet by the workflow, or an independent width-evaluation input. Geometry Results carry no implicit width field. Three explicit domains are whole-PathSet normalized arc, per-subpath normalized arc and per-subpath arc pixels. The generic ArcLength label does not distinguish these three domains. Binding schemas must represent source maps, closed seams and zero-length behavior.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-05A](PTH-05A_width_profile_linear.md) | width profile linear |
| [PTH-05B](PTH-05B_width_profile_pchip.md) | width profile pchip |
| [PTH-05C](PTH-05C_attach_linear_width.md) | pair the original path with a separate Linear width Result |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).
