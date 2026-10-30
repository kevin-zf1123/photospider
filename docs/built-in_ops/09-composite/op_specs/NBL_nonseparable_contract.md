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
