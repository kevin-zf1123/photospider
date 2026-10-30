---
spec_schema_version: 1
id: GRD-24
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-24: Composable automatic levels/exposure

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-24A](GRD-24A_selected_quantiles.md) | Quantiles of a selected feature | `grade.selected_quantiles` |
| [GRD-24B](GRD-24B_auto_exposure_parameters.md) | Derive automatic-exposure parameters | `grade.auto_exposure_parameters` |
| [GRD-24C](GRD-24C_auto_levels_parameters.md) | Derive automatic-levels parameters | `grade.auto_levels_parameters` |
| [GRD-24D](GRD-24D_auto_exposure.md) | Automatic exposure of a single image | `grade.auto_exposure` |
| [GRD-24E](GRD-24E_auto_levels.md) | Automatic levels in Proportional RGB luminance | `grade.auto_levels` |
| [GRD-24F](GRD-24F_auto_levels_xyz_y.md) | Automatic levels in XYZ Y | `grade.auto_levels_xyz_y` |
| [GRD-24G](GRD-24G_auto_levels_cielab_l.md) | Automatic levels in CIELAB lightness | `grade.auto_levels_cielab_l` |
| [GRD-24H](GRD-24H_auto_levels_oklab_l.md) | Automatic levels in OKLab lightness | `grade.auto_levels_oklab_l` |
| [GRD-24I](GRD-24I_auto_levels_channels.md) | Automatic per-channel levels | `grade.auto_levels_channels` |

## Shared mathematics and behavior

### Selection and metering

The static selection_mode is binary by default. Binary selection has UInt8 values 0/1 and assigns one unit of mass per selected pixel. Weighted selection has Float64 values in [0,1] and assigns that mass. ROI is a half-open integer rectangle [x0,y0,x1,y1] within the full image; its default is the full image and empty rectangles are legal. count is the number of positive-mass samples; mass is their weight sum. Positive-weight samples must satisfy the feature domain; zero-weight source samples have no statistical dependency.

Image-level auto operators additionally offer alpha_policy=include|exclude_zero|weight, default exclude_zero. Missing alpha acts as one. exclude_zero sets weights to zero where alpha=0; weight multiplies the selection by alpha and is allowed only with selection_mode=weighted. Alpha is an explicitly identified plane with values in [0,1]. Binary/weighted feature primitives receive an already constructed selection and have no implicit alpha input. A caller can therefore use the same effective selection for histogram/CLAHE and automatic adjustment.

### Quantile definition

Sort positive-weight samples by numerical value, retaining global row-major order on ties. For N binary samples and p in [0,1], h=(N-1)*p, i=floor(h), t=h-i; q=(1-t)*x[i]+t*x[min(i+1,N-1)]. Exact ranks and endpoints select samples. For weighted mode, let W be total weight and C_j the cumulative sorted weights. p=0 selects the first positive-weight sample; p=1 selects the last; otherwise select the first j with C_j>=p*W. This inverse-CDF estimator is intentionally distinct from the binary linear estimator, even for equal nonzero weights. Its mathematical definition follows the [weighted CDF reference](../research-sources.md#quantiles).

Empty selection returns count=mass=0 and documented zero placeholders. Parameter generators return state=1 for insufficient statistics; complete image operators copy original samples before any levels clamp or color reconstruction. Other runtime failures retain the foundation behavior.

### Automatic image entries

D performs linear-Y auto exposure. E/F/G/H are automatic levels using proportional RGB, XYZ Y, CIELAB l and OKLab L respectively. I applies independent levels per selected channel. All operate on a single image without temporal state. Metering ROI and effect/application ROI are independent; all requested image outputs use the same metering parameters.


## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
