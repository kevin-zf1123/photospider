---
spec_schema_version: 1
id: GRD-24
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
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
| [GRD-24D](GRD-24D_auto_exposure.md) | Automatic exposure of a single image | `grade.auto_exposure`<br>`grade.auto_exposure_statistics`<br>`grade.auto_exposure_apply` |
| [GRD-24E](GRD-24E_auto_levels.md) | Automatic levels in Proportional RGB luminance | `grade.auto_levels`<br>`grade.auto_levels_statistics`<br>`grade.auto_levels_apply` |
| [GRD-24F](GRD-24F_auto_levels_xyz_y.md) | Automatic levels in XYZ Y | `grade.auto_levels_xyz_y`<br>`grade.auto_levels_xyz_y_statistics`<br>`grade.auto_levels_xyz_y_apply` |
| [GRD-24G](GRD-24G_auto_levels_cielab_l.md) | Automatic levels in CIELAB lightness | `grade.auto_levels_cielab_l`<br>`grade.auto_levels_cielab_l_statistics`<br>`grade.auto_levels_cielab_l_apply` |
| [GRD-24H](GRD-24H_auto_levels_oklab_l.md) | Automatic levels in OKLab lightness | `grade.auto_levels_oklab_l`<br>`grade.auto_levels_oklab_l_statistics`<br>`grade.auto_levels_oklab_l_apply` |
| [GRD-24I](GRD-24I_auto_levels_channels.md) | Automatic per-channel levels | `grade.auto_levels_channels`<br>`grade.auto_levels_channels_statistics`<br>`grade.auto_levels_channels_apply` |

## Shared mathematics and behavior

### Selection and metering

The static selection_mode is binary by default. Binary selection has UInt8 values 0/1 and assigns one unit of mass per selected pixel. Weighted selection has Float64 values in [0,1] and assigns that mass. The metering rectangle is specified by `roi_x0:Int64=0`, `roi_y0:Int64=0`, `roi_x1:Int64=W`, `roi_y1:Int64=H`; these are half-open coordinates within the full image. Empty rectangles are legal. `selection_mode:String=binary|weighted` defaults to `binary`. count is the number of positive-mass samples; mass is their weight sum. Positive-weight samples must satisfy the feature domain; zero-weight source samples have no statistical dependency.

Image-level auto operators additionally offer alpha_policy=include|exclude_zero|weight, default exclude_zero. Missing alpha acts as one. exclude_zero sets weights to zero where alpha=0; weight multiplies the selection by alpha and is allowed only with selection_mode=weighted. Alpha is an explicitly identified plane with values in [0,1]. It is bound to the selected semantic group or supplied through optional `alpha_channel:Int64`; an explicit index asserts agreement with a selected-group binding. A different group's alpha and an otherwise unlabelled fourth channel are not inferred. `alpha_policy:String=exclude_zero|include|weight` has the default `exclude_zero`. Binary/weighted feature primitives receive an already constructed selection and have no implicit alpha input. A caller can therefore use the same effective selection for histogram/CLAHE and automatic adjustment.

### Quantile definition

Sort positive-weight samples by numerical value, retaining global row-major order on ties. For N binary samples and p in [0,1], h=(N-1)*p, i=floor(h), t=h-i; q=(1-t)*x[i]+t*x[min(i+1,N-1)]. Exact ranks and endpoints select samples. For weighted mode, let W be total weight and C_j the cumulative sorted weights. p=0 selects the first positive-weight sample; p=1 selects the last; otherwise select the first j with C_j>=p*W. This inverse-CDF estimator is intentionally distinct from the binary linear estimator, even for equal nonzero weights. Its mathematical definition follows the [weighted CDF reference](../research-sources.md#quantiles).

Empty selection returns count=mass=0 and documented zero placeholders. Parameter generators return state=1 for insufficient statistics; complete image operators copy original samples before any levels clamp or color reconstruction. Other runtime failures retain the foundation behavior.

### Static parameter encoding

Color selectors and `max_work` use the [GRD-21 parameter encoding](GRD-21_saturation_contract.md#static-parameter-encoding). D and its helpers accept `percentile:Float64=0.5` in [0,1], `target:Float64=0.18`, `ev_min:Float64=-16` and `ev_max:Float64=16`. B accepts the same target and EV bounds. E–I and their helpers accept `probability_low:Float64=0.01`, `probability_high:Float64=0.99` with 0<=low<high<=1, and `gamma:Float64=1`, `out0:Float64=0`, `out1:Float64=1`; C accepts the same gamma and output endpoints. Target and gamma are positive; ev_min<=ev_max. Probabilities in image entries are static scalars, not additional tensor ports. A's probability tensor remains dynamic; its entries may appear in any order, and the quantile output preserves that order.

The image component axis may be any one of the three logical axes; the remaining axes in ascending order are y,x. Numeric I accepts any nonempty ordered unique component subset, without a four-component limit or RGB-basis requirement. Its description entry retains the existing FMT color-group constraints.

### Automatic image entries

D performs linear-Y auto exposure. E/F/G/H are automatic levels using proportional RGB, XYZ Y, CIELAB l and OKLab L respectively. I applies independent levels per selected channel. All operate on a single image without temporal state. Metering ROI and effect/application ROI are independent; all requested image outputs use the same metering parameters.

### Explicit statistics/application prototypes

For each D–I main key, `_statistics(input,selection;P) -> controls:Float64[rows,6]` and `_apply(input,controls;P) -> values:T` are independently callable keys. The application retains the complete input shape and dtype. D–H have rows=1; I has one row per selected component in selection order.

D stores `[ev,0,0,0,0,state]`; insufficient statistics stores `[0,0,0,0,0,1]`. E–I store `[low,high,gamma,out0,out1,state]`; insufficient statistics stores `[0,1,1,0,1,1]`. Here state is Float64 with values {0,1}; the separate state output of B/C remains UInt8. State 1 copies source bits before clamping or reconstruction.

A complete image entry produces this explicit atomic controls result once in the current Run and shares it across all application tiles. A subsequent Run recomputes it from that Run's input and selection. The apply keys accept the same static parameter vocabulary P, take numerical adjustment values from controls, and perform no statistical scan.

G/H additionally use explicit Run-local `Float64[H,W,3]` temporaries containing original CIELAB coordinates or native LMS cube-root values for requested application coordinates, as specified by their current mathematical profiles. This does not add ports to the independently callable `_statistics` and `_apply` helpers.

The current G/H native root profile is a deterministic per-sample sequence shared by feature extraction, original coordinates, statistics, complete application and the independently callable helpers. On x86-64, a positive normal binary64 magnitude with unsigned encoding b starts from inverse-root encoding `(1364*2^52)-floor(b/3)`. Multiply this seed once by the exact binary64 value `123/128`, then four steps compute `d=fma(-(x*r),r*r,1)` and `r=fma(r*c,d,r)`, with the binary64 constant `c=0x1.5555555555555p-2`; the positive root is `(x*r)*r`. Negative inputs restore their sign. Zero, subnormal and nonfinite root arguments use the platform cube root. AArch64 uses the platform cube root for all arguments. Vector evaluation applies this same sequence independently to each sample, including exceptional fallback. CIELAB retains its existing linear branch and threshold. Published features determine exact ranks and stable row-major ties; copy decisions compare the resulting native coordinates exactly. The sequence is independent of neighbouring samples, packet size, requested ROI and tile partition. It obeys the unchanged native continuous comparison criteria and does not prescribe the native sequences of GRD-17, GRD-20, NUM or independently invoked FMT nodes.

The binary profile determines ranks exactly from binary64 probabilities, performs exact multiselection, and interpolates in Float64. A proposed sample interval is usable only after a complete count proves the requested ranks remain inside it; otherwise exact multiselection applies. Weighted selection compares exact binary weight mass thresholds and preserves row-major numeric ties. For a small probability set, a bounded three-way value partition may accumulate exact less/equal masses and carry the absolute prefix mass into each child interval; probabilities and total mass are never renormalized. A threshold reached at the less partition boundary remains in that partition. Exhausted partition work/depth and signed-zero ties fall back to full `(value,row-major index)` ordering. Equal nonzero interpolation endpoints remain constant.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
