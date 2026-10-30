---
spec_schema_version: 1
id: PTH-04
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-04: Resampling and flattening

Samples carry Float64 positions, source segment and independently rounded local t; arc variants additionally carry arc_s. No arc_s is invented for parameter-only sampling. Formal variants must bind fields to source snapshots. Empty paths are empty Results, M-only paths are valid points. Flattening is a deterministic approximation, not original-curve exactness.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-04A](PTH-04A_resample_uniform_t.md) | resample uniform t |
| [PTH-04B](PTH-04B_resample_uniform_arc.md) | resample uniform arc |
| [PTH-04C](PTH-04C_resample_arc_spacing.md) | resample arc spacing |
| [PTH-04D](PTH-04D_flatten_bezier_path.md) | flatten bezier path |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).
