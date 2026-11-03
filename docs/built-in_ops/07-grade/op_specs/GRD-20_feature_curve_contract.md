---
spec_schema_version: 1
id: GRD-20
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
---

# GRD-20: Feature curves and explicit target edits

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-20A](GRD-20A_feature_curve_periodic.md) | Periodic feature curve | `grade.feature_curve_periodic` |
| [GRD-20B](GRD-20B_feature_curve_linear.md) | Nonperiodic feature curve | `grade.feature_curve_linear` |
| [GRD-20C](GRD-20C_apply_hue_delta.md) | Apply a hue-increment field | `grade.apply_hue_delta` |
| [GRD-20D](GRD-20D_apply_chroma_scale.md) | Apply a chroma-scale field | `grade.apply_chroma_scale` |
| [GRD-20E](GRD-20E_apply_lightness_delta.md) | Apply a native lightness-increment field | `grade.apply_lightness_delta` |
| [GRD-20F](GRD-20F_reconstruct_luminance.md) | Set luminance by proportional RGB scaling | `grade.reconstruct_luminance` |
| [GRD-20G](GRD-20G_apply_saturation_scale.md) | Apply an HSL/HSV saturation-scale field | `grade.apply_saturation_scale` |
| [GRD-20H](GRD-20H_set_xyz_y.md) | Set XYZ Y while retaining X and Z | `grade.set_xyz_y` |
| [GRD-20I](GRD-20I_set_cielab_lightness.md) | Set CIELAB lightness while retaining a and b | `grade.set_cielab_lightness` |
| [GRD-20J](GRD-20J_set_oklab_lightness.md) | Set OKLab lightness while retaining a and b | `grade.set_oklab_lightness` |

## Shared mathematics and behavior

Periodic and nonperiodic source curves produce modulation with an explicit static unit: hue increment, chroma or saturation multiplier, model-lightness increment, or EV. C/D/E/G apply these fields; EV feeds GRD-01B. F/H/I/J are the four independent complete RGB brightness replacement routes. Static units are passed explicitly to consumers.

## Current parameters

The operation keys in the member table are the registered keys. Every image, feature, knot table, ordinate table and modulation/target field in a member signature is a required dynamic input.

| Parameter | Type | Current meaning |
| --- | --- | --- |
| `axis` | Int64 | Component axis. Required for raw tensor selection; an explicit input description may supply it. |
| `components` | String | Ordered, unique, comma-separated canonical nonnegative decimal indices, such as `"2,0,1"`. Raw A/B require `"0"` on a component axis of extent one. Raw C–J require three indices in the model order stated below. |
| `input_description` | String | Optional existing FMT tensor-description encoding, mutually exclusive with `axis` and `components`. An explicitly named group supplies the axis, component roles and color interpretation; explicit model, basis and unit assertions must agree. |
| `group` | String | Required explicit group selection when using an input description. |
| `modulation_unit` | String | Required for A/B: `delta_h`, `chroma_scale`, `saturation_scale`, `delta_lightness` or `ev`. Required for C/D/E/G with the respective value `delta_h`/`chroma_scale`/`delta_lightness`/`saturation_scale`. |
| `period` | Float64 | Required by A; positive finite period in the feature coordinate's unit. |
| `edge` | String | B only: `reject` or `clamp`, default `clamp`. Reject retains the common unchecked query-domain premise. |
| `model` | String | C: `hsl`, `hsv`, `cielch`, `oklch`; D/E: `cielab`, `oklab`; G: `hsl`, `hsv`. Required for raw selection; may be supplied by an agreeing described group. |
| `hue_unit` | String | Required by C: `radian` or `pi_multiple`, shared by source hue and delta. |
| `source_basis` | String | F/H/I/J: existing FMT RGB-basis codec, either a named preset or `basis-v1:` coordinates. Required for raw selection; a described relative linear RGB group may supply it. |
| `max_work` | Int64 | Optional nonnegative numerical work limit, default `INT64_MAX`. |

The selected three-coordinate order is H/S/L or H/S/V for HSL/HSV, l/C/h or L/C/h for CIELCh/OKLCh, l/a/b or L/a/b for CIELAB/OKLab, and R/G/B for the RGB routes. CIELAB lightness is normalized CIE l=L*/100; its a*, b* and C* coordinates retain their native scale. Field tensors have exactly the source sample shape with the selected component axis replaced by extent one; there is no implicit broadcasting.

## Current native mathematical profile

Consumed source values and controls are evaluated in binary64, followed by one conversion to the declared output dtype. A/B always return binary64. The common native comparison criteria and unchecked numeric-domain premises apply; this profile does not introduce strict intermediate rounding.

### Feature curves

A reduces the query into `[0,period)`. For `abs(query/period)<2^20`, use `fma(-floor(query/period),period,query)`; otherwise use native `fmod`. Add the period to a negative remainder and map a remainder at the upper endpoint to zero. Append the mathematical closing segment `(last_knot,last_ordinate) -> (period,first_ordinate)`.

A/B select the true containing interval and compute the slope `a=(y1-y0)/(x1-x0)`. For `chroma_scale` and `saturation_scale`, a descending interval (`y1<y0`) uses `fma(q-x1,a,y1)`; all other intervals use `fma(q-x0,a,y0)`. Anchoring scale interpolation at the smaller ordinate makes the product and anchor nonnegative for valid finite results, avoiding cancellation across zero. The signed units `delta_h`, `delta_lightness` and `ev` always retain the left-endpoint formula. IEEE overflow and invalid arithmetic continue to follow the common contract. Exact knots and clamped endpoints select stored ordinates. An interval whose two ordinates equal their corresponding knots returns q, and an interval with equal ordinates returns that constant; zero results of these interior shortcuts are positive zero, while stored knot/edge zeros retain their signs. Interval search may use a checked interpolation-position hint or a monotone bucket guide followed by binary search. These hints never replace comparisons against the actual knots. Slopes may be prepared from the current dynamic tables or evaluated for the selected interval.

### Coordinate edits

C adds delta to the hue only when the original saturation/chroma and delta are both nonzero. D multiplies a and b by scale. E adds delta to l/L. G multiplies S by scale and clamps the result to `[0,1]`. All these operations retain the specified identity branches and independently copied coordinates, including their original bit patterns.

### Complete RGB brightness edits

Let M be the explicit RGB-to-XYZ matrix and N its inverse. Native three-term products use a left-to-right first product followed by two explicit FMA accumulations. F and H extract `Y=dot(M[1],rgb)`. F uses the stated proportional or neutral reconstruction. H uses the equivalent retained-X/Z form `rgb'[c]=fma(N[c,1],target-Y,rgb[c])`.

I computes `f=cie_f(M*rgb/white)` and `l=1.16*f.y-0.16`. Replacing l while retaining a,b is equivalent to adding `(target+0.16)/1.16-f.y` to all three f coordinates, applying the piecewise CIE inverse, multiplying by white and then by N. An explicit CIELAB coordinate conversion is also used under the same native profile.

For J, let A be Bradford adaptation from the source white to D65, U the XYZ-to-LMS matrix, and V the LMS-root-to-OKLab matrix. Compose `P=U*A*M`, `Q=N*A^-1*U^-1`, and `v=V^-1[:,0]`. Compute `r=cbrt(P*rgb)`, `L=dot(V[0],r)`, `d=target-L`, `t=v*d`, and component-wise `delta_cube=t*fma(t,t,fma(3*r,t,3*r*r))`. Then `rgb'=rgb+Q*delta_cube`. This difference-of-cubes form retains a,b mathematically.

Cube roots use ordinary scalar library evaluation or the native exponent-reduction kernel with a quadratic seed and three Halley refinements, with scalar handling for exceptional ranges. Before an approximate I/J feature can affect an exact-identity decision, a neighborhood of width `2^-48*max(1,scale)` is reevaluated using the scalar extraction sequence. The scale is `1.16*abs(f.y)+0.16` for I and `sum(abs(V[0,c]*r[c]))` for J, so cancellation in signed/HDR inputs does not narrow the identity check. Only exact equality to that scalar feature selects the original RGB bits. Other unchanged components retain their original bits independently.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
