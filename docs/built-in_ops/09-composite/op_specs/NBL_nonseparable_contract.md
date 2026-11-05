---
spec_schema_version: 1
id: NBL-common
kind: shared_operator_contract
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# NBL: native model and W3C color-property functions

Inherit [COM common](COM_common_contract.md). These primitives consume color-only triples, and return triples in the same model. Source/background conversion, white adaptation, gamut mapping and alpha composition are explicit surrounding nodes.

## Native CIELAB and OKLab

Let B=(L_B,a_B,b_B), S=(L_S,a_S,b_S), CB2=a_B^2+b_B^2, CS2=a_S^2+b_S^2. L denotes normalized CIE l or native OKLab L. The required threshold e>=0 for hue/chroma is in the respective opponent units; compare squared chroma with e^2. Threshold is an artistic control, not a numerical error tolerance.

| Property | Equation |
| --- | --- |
| hue | If CS2<=e^2 copy B. Otherwise return (L_B,a_S*sqrt(CB2/CS2),b_S*sqrt(CB2/CS2)); when CB2=0 the generated opponents are +0 |
| chroma, nonneutral B | Return (L_B,a_B*sqrt(CS2/CB2),b_B*sqrt(CS2/CB2)) |
| color | Copy (L_B,a_S,b_S) |
| lightness | Copy (L_S,a_B,b_B) |

For chroma when CB2<=e^2, independent members define three policies: `preserve_gray` copies B; `source_hue` copies (L_B,a_S,b_S); `zero_hue` returns (L_B,sqrt(CS2),+0). Zero hue is the positive native a axis, not an asserted pure RGB red. At zero threshold these cases concern exact achromatic values; a positive threshold also classifies a small near-neutral region and may create a branch discontinuity.

Copied coordinates preserve bits. Copy color/lightness only reads its selected coordinate from the corresponding input. Hue and chroma require both opponent pairs and threshold to determine the branch and radial scaling; output L reads background L only. Neither function uses a spatial neighborhood.

## W3C RGB constructions

For bounded RGB, define Lum(C)=0.30R+0.59G+0.11B and Sat(C)=max(C)-min(C). These weights define the W3C artistic coordinate and are distinct from working-basis linear Y.

ClipColor(C) captures l=Lum(C), n=min(C), x=max(C). If n<0 replace each component by l+(C-l)*l/(l-n). If x>1 replace the current components by l+(C-l)*(1-l)/(x-l), using the original captured x,l. SetLum(C,l_target)=ClipColor(C+(l_target-Lum(C))).

SetSat(C,s) sorts components by (value,RGB index). For max>min set the middle component to (mid-min)*s/(max-min) and the maximum to s; otherwise both are zero. Set the minimum to zero, then restore original component order.

| Property | Construction |
| --- | --- |
| hue | SetLum(SetSat(S,Sat(B)),Lum(B)) |
| saturation | SetLum(SetSat(B,Sat(S)),Lum(B)) |
| color | SetLum(S,Lum(B)) |
| luminosity | SetLum(B,Lum(S)) |

The strict oracle evaluates each full construction before output rounding; native evaluation uses ordinary floating intermediates. Executed denominators are nonzero on the stated domain. W3C outputs require both full tuples at the same coordinate. See the [W3C source](../research-sources.md#coverage-and-color-blending) for these public definitions.

## Parameters and native arithmetic profiles

The sixteen member specifications define the operation keys and ordered tensor inputs. Static parameters use the following encoding:

| Parameter | Type | Current meaning |
| --- | --- | --- |
| `axis` | Int64 | Component axis within the tensor cell, excluding leading batch axes, in `[0, cell_rank)`. Required with raw selectors. |
| `components` | String | Required with `axis`: a permutation of `"0,1,2"` giving the three coordinates in model order. The selected axis has extent 3. This String encodes COM's logical ordered selector array. |
| `input_description` | String | Existing FMT tensor-description encoding; may provide axis, model, component roles, common white and working domain instead of raw selectors. |
| `group` | String | Required color-group name with a component-axis `input_description`. Select either a description and group or raw `axis` and `components`. |
| `source_white` | String | CIELAB members only: the explicitly shared white, using the FMT white encoding, for example `"d65"`. Required when a selected description does not provide a white; when both are supplied they must agree. The inputs share this white. |
| `working_domain` | String | W3C members only: `"linear"` or `"encoded"`. Required without a description and checked against a supplied description. The inputs share this domain. |
| `max_work` | Int64 | Optional bound on numerical work per callback, in `[0, INT64_MAX]`; default `INT64_MAX`. Resource failures follow the [shared resource contract](../../00-foundation/contracts.md#资源与生命周期). |

`neutral_threshold` is a required same-dtype tensor input of shape `[1]` for native CIELAB/OKLab Hue and Chroma members. Its value is finite and nonnegative in native opponent units. A request for only the L coordinate depends on background L. W3C members take only the two image inputs. Static selectors and descriptions determine the interpretation of those inputs.

### Native opponent arithmetic

For each opponent pair, the native squared norm is `q = RN_T(RN_T(a*a) + RN_T(b*b))`. Radial scaling evaluates the selected native quotient, its native square root, and the component multiplication in that order. The zero-hue neutral result evaluates the source norm's square root directly. These continuous stages retain COM's ordinary overflow, invalid-arithmetic and underflow behavior.

The neutral branch is decided from the exact squares of the stored inputs, including subnormal and very large finite values. At zero threshold it compares both opponents directly with zero. For positive thresholds, compare `a*a + b*b <= threshold*threshold` exactly for the stored binary values. Rounding or overflow of the native squared norm `q` leaves this discrete decision unchanged. Copy branches and the explicit generated zero opponents retain their bit rules.

### Bounded W3C arithmetic

Lum uses T-rounded decimal coefficients. The AArch64 profile evaluates `fma(T(.11),B,fma(T(.59),G,T(.30)*R))`; other CPU paths evaluate `(T(.30)*R + T(.59)*G) + T(.11)*B`. SetSat selects minimum, middle and maximum, multiplies `(middle-minimum)` by the target saturation before division, and assigns its endpoints directly. Its computed middle value is bounded to `[0,target]` to remove native rounding overshoot.

Both W3C quotient stages use native T division. The AArch64 profile also uses an explicit fused multiply-add for `l + (component-l)*scale` in ClipColor. Within each profile, a requested coordinate uses the same arithmetic graph across layouts and tile partitions. These fused stages follow COM's native continuous comparison policy; they do not change the exact neutral predicate or bit-preserving copies. Opponent norms and radial scaling retain their separately rounded stages.

The native SetLum profile uses the real-arithmetic identity `Lum(C + (target - Lum(C))) = target`, since the exact W3C weights sum to one. It bounds the stored target to `[0,1]`, uses that value for the captured luminosity, and computes the shifted components and their original extrema before correction. This continuous rearrangement follows COM's native comparison tolerances; its intermediate rounding is defined by the operations above.

After SetSat, the minimum is exactly zero and the maximum is the target saturation when the original tuple was nonconstant, or zero when it was gray. The native profile propagates these extrema through the monotone common offset, using `n=delta` and `x=RN_T(effective_maximum+delta)`. Other SetLum inputs determine extrema from their shifted components.

SetLum's input is bounded, so the offset cannot produce both `n<0` and `x>1`; the native profile selects the applicable numerator and denominator and performs one correction. With captured luminosity in `[0,1]`, a lower correction has `l-n>0` and an upper correction has `x-l>0`, including gray black/white limits. Their ratios lie in `[0,1]`. The final ClipColor result is bounded to `[0,1]` to remove native rounding overshoot. These bounds belong to the stated W3C construction and do not apply a gamut mapping to native CIELAB/OKLab members.
