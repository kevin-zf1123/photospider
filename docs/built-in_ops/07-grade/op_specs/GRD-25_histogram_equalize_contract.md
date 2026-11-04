---
spec_schema_version: 1
id: GRD-25
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
---

# GRD-25: Histogram equalization and CLAHE

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-25A](GRD-25A_histogram.md) | Selected histogram | `grade.histogram` |
| [GRD-25B](GRD-25B_histogram_cdf_map.md) | Histogram CDF mapping | `grade.histogram_cdf_map` |
| [GRD-25C](GRD-25C_histogram_apply.md) | Apply a histogram map | `grade.histogram_apply` |
| [GRD-25D](GRD-25D_clahe_maps.md) | Generate local CLAHE maps | `grade.clahe_maps` |
| [GRD-25E](GRD-25E_clahe_apply.md) | Apply local CLAHE maps | `grade.clahe_apply` |
| [GRD-25F](GRD-25F_histogram_equalize.md) | Complete histogram equalization | `grade.histogram_equalize` |
| [GRD-25G](GRD-25G_clahe.md) | Complete CLAHE | `grade.clahe` |

## Shared mathematics and behavior

Binary and weighted selection use [GRD-24](GRD-24_auto_adjust_contract.md). Feature inputs carry an explicit effective selection, including any alpha weighting constructed upstream. Binning is over [0,1] after the member edge rule. Output selection is independent of the statistical selection.

### Static parameter vocabulary

All members accept `max_work:Int64=9223372036854775807`. Their remaining static keys are:

| Members | Static parameters |
| --- | --- |
| A/F | `selection_mode:String=binary|weighted`, default binary; `edge:String=clamp|reject`, default clamp; `B:Int64=256` in 2..65536; `roi_x0:Int64=0`, `roi_y0:Int64=0`, `roi_x1:Int64=W`, `roi_y1:Int64=H` |
| B | `selection_mode:String=binary|weighted`, default binary |
| C | `edge:String=clamp|reject`, default clamp |
| D/G | `selection_mode`, `edge`, `B` as above; `cell_width:Int64=64`, `cell_height:Int64=64`, both >=1; `clip_factor:Float64=2`, >=1 |
| E | `edge`, `cell_width`, `cell_height` as above |

B/C/E infer B from the input vector/map extent. D/G have no metering-ROI parameters; each requested logical cell uses its complete source rectangle. A/F metering coordinates are half-open and may be empty.

### CLAHE clipping and redistribution

Let N be cell count or total positive weight, B the number of bins and h_i the bin counts/masses. Empty or single-positive-bin cells use identity before clipping. For binary mode, L=max(1,floor(clip_factor*N/B)); c_i=min(h_i,L), E=N-sum(c). Add floor(E/B) to every bin, then one more to bins i<E mod B. For weighted mode, L=clip_factor*N/B; c_i=min(h_i,L); add (N-sum(c))/B to every bin. Both perform one redistribution step and may exceed the initial cap afterward. Form map_i=prefix(c)_i/N, without subtracting the first occupied cumulative value. Global CDF mapping in B uses its separately stated minimum-CDF correction.

Cells are anchored to the full-image origin. Their centers are the midpoints of their actual cropped geometric extents. Empty/constant cells contribute the identity transform to interpolation. Logical cell geometry and output tile geometry are independent. These explicit floating-input definitions establish native CLAHE behavior.


## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
