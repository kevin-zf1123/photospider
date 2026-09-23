---
spec_schema_version: 1
id: FMT-11-math
kind: shared_mathematical_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-11: model conversion mathematics

Inherit [the family decisions](FMT-11_model_conversion_contract.md) and the
[normalized CIE-lightness revision](FMT_relative_coordinate_scale.md).

## A/B: relative XYZ and CIELAB

Resolve the source white xy under the existing exact geometry rules. With
W=(xw/yw, 1, (1-xw-yw)/yw), all W components are strictly positive.
All literal fractions below are exact, not rounded decimal approximations.

Let delta=6/29 and epsilon=delta^3. Define

    f(t) = cbrt(t)                 if t > epsilon
           (841/108)*t + 4/29     otherwise

    g(u) = u^3                    if u > delta
           (108/841)*(u - 4/29)  otherwise

The branches agree at each exact threshold. Use the real positive cube root in
the forward high branch. Negative t belongs to the low linear branch. g is the
inverse of this extended f on the reals.

A has the complete finite formulas

    l = (116*f(Y) - 16)/100
    a* = 500*(f(X/Wx) - f(Y))
    b* = 200*(f(Y) - f(Z/Wz))

B has

    fy = (100*l + 16)/116
    X = Wx*g(fy + a*/500)
    Y = g(fy)
    Z = Wz*g(fy - b*/200)

Strict rounds each requested complete real output formula once to the tensor
dtype, nearest/ties-to-even. Branch comparisons and resolved white ratios are
exact. Do not separately round f, fy, ratios, differences or white products.
No intermediate machine overflow may fail a representable final result.
Accelerated accuracy and exact discrete decisions inherit NUM/FMT. Raw special
values follow the classification section below.

Here l=L*/100 is the revised native storage coordinate; the exact multiplication
by 100 is inside B's complete formula, not an extra rounded conversion node.
Source support: A l uses only Y; a* uses X,Y; b* uses Y,Z. B X uses l,a*;
Y uses l; Z uses l,b*. Outputs not requested are not evaluated. Dirty mapping
is the forward relation of these component dependencies at the same coordinate.

Analytic acceptance anchors, before output rounding:

- XYZ=(0,0,0) maps to Lab=(0,0,0), and conversely.
- Exact XYZ=W maps to normalized Lab=(1,0,0); actual stored inputs are interpreted at
  their exact binary values, so an independently rounded W needs its own oracle.
- For white xy=(0.25,0.25), W=(1,1,2) is exactly representable. XYZ=(1,1,2)
  maps exactly to (1,0,0), and Lab=(1,0,0) maps exactly to (1,1,2).
- For the same white, XYZ=(-1,-1,-2) maps to (-24389/2700,0,0) before rounding.
  This fixture checks the negative linear branch without clipping.
- At t=216/24389, f(t)=6/29 and l=2/25. Neither that t nor l is exactly
  representable in binary; test floating neighbors with an exact-input oracle.
  Do not pretend RN_dtype(0.08) lies exactly on B's mathematical threshold.

Source: [CIE e-ILV 17-23-076](https://cie.co.at/eilvterm/17-23-076), including
the rational low branch. g is its algebraic inverse. Extended signed/HDR use,
rounding and demand are Photospider decisions; they imply no CSS or CMM match.

## C/D and G/H: Cartesian and polar coordinates

The same geometry applies separately to CIELAB/CIELCh(ab) and OKLab/OKLCh,
without mixing their opponent scales or white definitions. Their first
coordinate (normalized l for CIELAB, L for OKLab) copies its
stored bits. For finite forward inputs:

    C = sqrt(a*a + b*b)
    theta = +0                    if a == 0 and b == 0
            atan2(b,a)           otherwise
    h = theta                    for radian
        theta/pi                 for pi_multiple

The principal branch includes both -pi and +pi distinguished by signed b=0 on
the negative real axis. Test exact zero before atan2 only for the joint-zero
case; a nonzero tiny pair is not achromatic. No epsilon is introduced.

For inverse finite inputs, theta is the exact stored radian value or the exact
stored pi_multiple value times mathematical pi:

    a = C*cos(theta)
    b = C*sin(theta)

Semantic C=0 validates finite hue and yields +0 for each requested a/b instead
of retaining trigonometric product signs. Semantic C<0 is invalid. Raw uses the
numerical products, including signed/negative C; it does not apply that semantic
zero-chroma output override. Exact trig zero/sign rules inherit NUM, including
pi_multiple evaluation without prematurely rounding multiplication by pi.

Strict rounds the complete C/h/a/b formula once; L stays an exact copy. Thus
forming squares or products in machine precision and overflowing prematurely is
not a valid reference. Finite nonzero underflow follows NUM. C/h each depend on
both a,b; a/b each depend on C,h even at C=0; L is independent. Semantic finite
validation follows these exact sets. No alpha or unrelated group participates.

Fixtures include a=3,b=4 giving C=5; a=-1,b=+0/-0 giving pi_multiple h=+1/-1;
all four signed-zero pairs giving C=+0,h=+0; and inverse C=2,h=5/2 in
pi_multiple giving a=0,b=2 under NUM exact special-angle signs. That inverse
loses its extra winding on the next forward transform. Test zero C with invalid
hue in semantic mode and L-only requests with invalid C/h to establish support.

## E/F: fixed XYZ and OKLab transform

The two forward matrices, in row-major notation, are:

    M1 = [ 0.8190224379967030  0.3619062600528904 -0.1288737815209879
           0.0329836539323885  0.9292868615863434  0.0361446663506424
           0.0481771893596242  0.2642395317527308  0.6335478284694309 ]

    M2 = [ 0.2104542683093140  0.7936177747023054 -0.0040720430116193
           1.9779985324311684 -2.4285922420485799  0.4505937096174110
           0.0259040424655478  0.7827717124575296 -0.8086757549230774 ]

Each printed decimal is an exact rational. E is M2*cbrt(M1*XYZ), with real
componentwise cube root, including negative LMS. F is
inverse(M1)*cube(inverse(M2)*OKLab), with exact matrix inverses. Each requested
complete output formula is rounded once under strict, without separately
rounded LMS or cube-root tensors. Exact matrix sums use the NUM dot convention.
Intermediate magnitude alone cannot cause overflow failure if the final exact
result rounds to a finite value; resource exhaustion remains possible.

Both directions read the complete source triple for each requested transformed
component. Neither reads alpha or skips hidden color. Finite signed/HDR input
and finite requested results are required in semantic mode. Both matrices are
invertible; absent storage rounding the finite real formulas invert one another.
That does not imply bitwise round trips, a perfectly neutral rounded white, or
invertibility after floating overflow or underflow.

The coefficient source is the fixed
[W3C snapshot](https://www.w3.org/TR/2026/CRD-css-color-4-20260913/#color-conversion-code).
The original [author derivation](https://bottosson.github.io/posts/oklab/)
explains the XYZ/LMS/cube-root structure. This contract explicitly selects the
W3C forward coefficients and exact inverses instead of mixing published tables.

## I/J/K/L: HSL and HSV finite algebraic extension

Use exact M=max(R,G,B), m=min(R,G,B), d=M-m. Let V=M, L=(M+m)/2.
Extrema inherit NUM minimum/maximum signed-zero rules: mixed zeros select -0
for m and +0 for M; same-sign zeros keep that sign. V is the exact selected
maximum value. All-negative-zero RGB therefore gives V=-0 and L=-0, with the
explicit H/S=+0 achromatic exception; mixed zero signs give L=+0. Hue sector
tie priority below does not override these numeric zero-selection rules.
For d=0 set H=+0,S=+0. Otherwise let the six-sector hue coordinate q be:

    q = ((G-B)/d) mod 6    when R==M
        (B-R)/d + 2       otherwise when G==M
        (R-G)/d + 4       otherwise

Ties use R, then G, then B priority; equivalent endpoint values are interpreted
on the same exact periodic circle. mod is Euclidean. Output H=q*pi/3 in radian
or q/3 in pi_multiple. Saturation is d/(1-abs(2*L-1)) for HSL and d/V for HSV.
A non-gray zero denominator fails only a requested semantic S. H and L/V do not
evaluate saturation. Each forward H,S,L/V still reads all RGB to resolve extrema
and validate its consumed triple. There is no negative-saturation hue rotation.

Inverse hue evaluation uses exact q=(3*H/pi) mod 6 for radian, or (3*H) mod 6
for pi_multiple. Let c=(1-abs(2*L-1))*S and offset=L-c/2 for HSL;
c=V*S and offset=V-c for HSV. Let v=c*(1-abs((q mod 2)-1)). Choose:

| q interval | RGB before adding offset to each component |
| --- | --- |
| [0,1) | c,v,0 |
| [1,2) | v,c,0 |
| [2,3) | 0,c,v |
| [3,4) | 0,v,c |
| [4,5) | v,0,c |
| [5,6) | c,0,v |

Every requested inverse component uses the full H,S,L/V validation support even
when c=0. Finite S/L/V extensions are not clipped. Such arbitrary inverse inputs
need not return the original H/S on a forward round trip: sector degeneracy,
negative c and zero denominators limit invertibility. Complete formulas round
once under strict; sector decisions use exact values and Euclidean reduction,
including very large finite hues. No whole-image scan or hue unwrapping occurs.

Fixtures: RGB=(1,0,0) -> HSL=(0,1,0.5), HSV=(0,1,1); RGB=(0,1,0) gives H=2/3
in pi_multiple; gray=(2,2,2) gives H=S=0 and L/V=2. RGB=(1,0,-1) has L=0 and
undefined finite HSL S, but H and L requests succeed. RGB=(0,-1,-2) likewise has
V=0 and undefined finite HSV S. Use actual stored floating hue for inverse
oracles; rounded 2/3 is not the exact rational 2/3 in a later call.

The nominal HSV six-sector construction is described by
[Alvy Ray Smith](https://alvyray.com/Papers/CG/hsv2rgb.htm). The explicit signed/HDR
extension and singularity choices above are this family's contract, not a claim
about that nominal-range implementation.

## M/N: native NCL coordinates

Kr,Kb are fixed Float64 descriptor coordinates. Presets expand to RN64 of
BT.601 (.299,.114), BT.709 (.2126,.0722), BT.2020 NCL (.2627,.0593), matching
the existing NCL descriptor presets. After resolution use the exact binary
rationals Kr,Kb and Kg=1-Kr-Kb. Require Kr>0,Kb>0,Kg>0 exactly in all modes.
Preset names choose only these coefficients, not transfer or primaries.

M, with no separately rounded intermediate Y':

    Y' = Kr*R' + Kg*G' + Kb*B'
    Cb = (B'-Y')/(2*(1-Kb))
    Cr = (R'-Y')/(2*(1-Kr))

N, with no separately rounded reconstructed R'/B' inside G':

    R' = Y' + 2*(1-Kr)*Cr
    B' = Y' + 2*(1-Kb)*Cb
    G' = Y' - (2*Kb*(1-Kb)/Kg)*Cb - (2*Kr*(1-Kr)/Kg)*Cr

Strict computes the exact expanded linear form plus +0 bias and rounds once.
The expanded forward rows, also used for raw Inf classification, are:

    [ Kr,                 Kg,                  Kb   ]
    [-Kr/(2*(1-Kb)),     -Kg/(2*(1-Kb)),        1/2  ]
    [ 1/2,              -Kg/(2*(1-Kr)),       -Kb/(2*(1-Kr)) ]

Do not compute an intermediate Y' and subtract infinite terms for raw Cb/Cr;
that would be a different NUM expression. For example raw (0,0,+Inf) gives
Y'=+Inf,Cb=+Inf,Cr=-Inf, rather than an Inf-Inf chroma NaN.
M consumes the full triple; N support is R'<-Y',Cr, B'<-Y',Cb, G'<-Y',Cb,Cr.
The latter two-component supports do not execute omitted zero-coefficient terms.
Coefficient quantization precedes these equations only as the shared Float64
metadata representation; do not round the derived matrix again to sample dtype.

Fixtures: for any admitted coefficients, equal RGB=(v,v,v) yields (v,0,0)
mathematically and N(Y',0,0) yields equal RGB. Kr=Kb=0.25 gives Kg=0.5;
RGB=(1,0,0) yields (0.25,-1/6,0.5) before rounding. Validate round trips against
each stage's rounded input, not against an unrounded symbolic -1/6.

Coefficient/formula sources:
[BT.601-7](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.601-7-201103-I!!PDF-E.pdf),
[BT.709-6](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.709-6-201506-I!!PDF-E.pdf),
[BT.2020-2](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2020-2-201510-I!!PDF-E.pdf).
Their transport quantization and subsampling do not become operator behavior.

## O/P: XYZ and xyY

O copies Y. For x/y, compute exact T=X+Y+Z then x=X/T,y=Y/T. In semantic mode
only, an all-zero triple substitutes source reference-white xy; a nonzero triple
with exact T=0 fails. Raw applies the ratios without the black substitution.
P copies Y; its other outputs are X=x*Y/y and Z=(1-x-y)*Y/y. Semantic X/Z
requests require y!=0 even at Y=0. Raw retains NUM division behavior.
Complete finite formulas round once, avoiding intermediate overflow, underflow
or a rounded denominator changing a branch. Copied Y preserves its zero sign.

O x/y read all XYZ; P X/Z read all xyY. Y copies in either direction only read
the corresponding Y. These supports apply to validation and forward dirty
mapping. Scale/reference/observer survive; absolute cd/m² Y is not divided by a
reference luminance. Sample x/y need not satisfy reference-white constraints.

Fixture W=(1,1,2) maps to (0.25,0.25,1), with exact inverse. Semantic XYZ black
with that white produces (0.25,0.25,Y_signed_zero). XYZ=(1,-1,0) makes x/y fail
but Y-only returns -1. xyY=(0.5,0,0) fails semantic X/Z even at zero Y.
XYZ=(1,0,1) is an admitted signed/extended XYZ and maps to (0.5,0,0); this
demonstrates that forward success alone does not guarantee a valid inverse.
Another loss case is valid white xy=(1/4,2^-150): Float32 semantic black output
rounds its y to +0 (a half-minimum-subnormal tie), so subsequent P X/Z fails.
There is no implicit minimum-white clamp; ordinary representable D50/D65 black
fixtures still reconstruct normally.

The ratio definition follows [CIE chromaticity coordinates](https://cie.co.at/eilvterm/17-23-053).
The black fallback, singular errors and exact request scope are explicit project
choices. No physical-color membership or observer transformation is performed.

## Q/R: Gray coordinates and neutral reconstruction

Q selects the native coordinate Y from XYZ, Y' from NCL YCbCr, l from CIELAB
or L from OKLab and copies its bits. R reconstructs the corresponding neutral
tuple: (Wx*Y,Y,Wz*Y), (Y',+0,+0), (l,+0,+0), or (L,+0,+0). For linear Y,
the white ratios are resolved exactly as in A/B. R preserves relative/absolute
Y scale rather than choosing a reference luminance. Perceptual lightness remains
on its original relative scale, and luma retains its underlying encoded RGB.

Computed R X/Z round once; the directly reused coordinate copies bits. Q/R do
not include a RGB conversion or implicitly decode an encoded luminance Gray.
In particular weighted luma Y' is not generally the transfer encoding of the
original color's luminance Y. The neutral tuple is a specified reconstruction.

Fixture: relative XYZ=(1,0.5,2) under white xy=(0.25,0.25) gives Gray Y=0.5;
R gives XYZ=(0.5,0.5,1), deliberately discarding source chroma. Lab=(0.5,20,-10)
gives Gray l=0.5, whose reconstruction is Lab=(0.5,0,0), not linear RGB=(0.5,0.5,0.5).
OKLab L reconstruction uses opponent a=b=0 exactly; the selected matrix's white
residual remains observable on further XYZ conversion rather than being fixed.

For a seven-channel tensor with selected XYZ slots [1,5,3], Q's output channel
sources are [0,5,2,4,6]. Gray is slot 1, and an alpha originally in slot 2 stays
slot 2. R expands that Gray to slots 1,2,3 and moves this alpha to slot 4. Test
these references independently of pixel arithmetic and keep unrelated order.

## S/T: binary Black/White

S has required finite Float64 threshold in the native Gray coordinate unit.
Compare the exact source floating value with the exact threshold value:
x>=threshold selects +1, otherwise +0. The chosen endpoints are represented
exactly in the source/output dtype. No source-to-Float32 coercion precedes a
Float64 comparison. Semantic S requires a finite consumed Gray sample; raw
uses the NUM comparison rules, including unordered NaN yielding false for >=.

T reads the selected binary sample, requires exactly 0 or 1 (either zero sign
counts as 0), then copies the selected finite same-dtype black_value/white_value
bits. It always validates the selector, including when the two constants are
equal, and includes that sample in exact demand/dirty support. Illegal selectors
fail in all modes. No implicit interpolation, coverage composition, transfer or
integer encoding occurs. S/T pass-through channels are independent.

Fixture: Gray l=[0.25,0.5,0.75], threshold=0.5 -> binary=[0,1,1]. T with levels 0,1
returns Gray l=[0,1,1], not the original samples. Reversed levels 1,0 and
equal levels 0.5,0.5 remain legal. T selector 0.25, NaN or Inf fails even for equal
levels. Input -0 selects the exact black constant, including its sign bit.

## Raw special values and evaluation order

Copies (bypass, polar lightness, xyY Y, Q, R's reused coordinate) preserve bits;
they do not quiet NaN. Constant R outputs do not read Gray. T always validates
its exact binary selector; S instead performs an ordered >= comparison, so NaN
selects +0, +Inf selects +1 and -Inf selects +0 for its finite threshold.

For other computed outputs, first inspect exactly their declared source support
in source model order. The first source NaN is quieted with its sign/payload
preserved under NUM, regardless of later algebraic cancellation. Unused peers
cannot supply a NaN. With no source NaN, evaluate the displayed expression tree
using exact finite quantities and IEEE extended-real classifications for Inf,
zero, invalid arithmetic and division. No finite intermediate is rounded or
overflowed to Inf. Generated invalid results use NUM's fixed positive quiet NaN.

Matrix sums, including expanded M/N rows rather than intermediate luma
subtraction, use the stated dot support and +0 bias; an infinite product has
the XOR of coefficient/input signs. Mixed opposite infinite terms are invalid.
E/F apply these sums, real cbrt/cube, then the second exact sum, without rounded
intermediate tensors. cbrt(+/-Inf)=+/-Inf and cube preserves infinity sign.
A/B f/g use their displayed comparisons, which place -Inf on the low branch.
Non-gray raw saturation divides even with a zero denominator. O never uses the
semantic black fallback, so raw zero/zero ratios yield canonical NaN. P raw
X/Z evaluate numerator then divide by y, including y=0.

C/G raw chroma follows sqrt(a*a+b*b). For hue, exact joint finite zero retains
the confirmed +0 rule; otherwise atan2 has the usual signed-axis limits. Finite
b with a=+Inf gives signed zero of b; a=-Inf gives signed pi. Infinite b with
finite a gives signed pi/2; with both infinite, signs select pi/4 or 3*pi/4
and the sign of b. Apply exact hue-unit conversion before final rounding.
D/H raw uses the displayed trigonometric products even for zero/negative C;
infinite hue is invalid, and zero C does not hide it. I/K extrema use model-order
tie priority and the displayed formulas. J/L infinite hue makes periodic reduction
invalid even when saturation/value would otherwise remove its influence.

All finite branches and sector predicates are exact. Computed exact cancellation
is +0, positive factors preserve a multiplicative zero sign, and explicit
trigonometric/atan2 signs take precedence. These rules specify deterministic
raw results without admitting nonfinite samples as semantically valid colors.
