---
spec_schema_version: 1
id: FMT-09-math
kind: mathematical_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-09 scalar transfer definitions

Inherit [FMT-09](FMT-09_transfer_contract.md). D is A decoding and E is B encoding.
The formulas below specialize the confirmed curve choices. They do not perform
basis conversion, normalization, storage quantization or view rendering.

## Evaluation conventions

Published terminating decimals denote exact rational constants. Static numeric
parameters denote their exact supplied Float64 values. For finite strict inputs,
select branches using exact comparisons and evaluate the entire chosen scalar
formula as a real expression, then round once to the unchanged output dtype
under NUM. Derived coefficients, gamma reciprocals and logarithms are not
implicitly rounded to Float64 intermediates. Only the selected branch executes;
a mathematically inactive log/exp/power branch cannot cause overflow/domain
failure. Accelerated precision and diagnostics inherit NUM's final-output
contract. Branch/domain decisions and all explicit copy results remain exact.

Semantic mode checks requested inputs and final results for finiteness, plus
any curve-specific domain. A finite result rounding to infinity fails; there is
no extra output clipping. Intrinsic max/floor/saturation below is part of the
curve and remains in raw mode. Raw removes sample semantic checks, retaining
NUM nonfinite/domain outcomes and static parameter checks. In nonidentity evaluations, NaN inputs follow
NUM propagation before ordered branch comparisons; they cannot accidentally
fall into a finite cap branch. Nonfinite inputs other than NaN may select a
formula-intrinsic constant branch, such as the ACES decoding upper cap.

For sRGB, gamma, BT.709 and BT.2020, use u=abs(x) in the listed positive-domain
formula, then restore the input sign, including -0. Other curves do not inherit
that extension. Linear identity copies all bits. Outside explicit copy/signed
rules, exact zero results are +0; formula values that are nonzero before rounding
use NUM's ordinary signed underflow. Nonfinite outcomes use NUM's defined
power/log/sqrt behavior, not an arbitrary complex or rational-root extension.

## Linear and power gamma

Linear: D(x)=E(x)=x, bit-preserving identity with unchanged native units/reference.
Gamma requires finite static gamma>0, with no implicit default exponent:

- D(u)=u^gamma.
- E(u)=u^(1/gamma).

Gamma=1 copies all input bits under the family identity rule, before raw NaN
propagation or formula evaluation. Semantic finite-input checks still apply. Parameter fields not applicable to a curve are
rejected rather than silently ignored.

## sRGB

Use the IEC-style piecewise normalized curve documented by
[ICC](https://registry.color.org/rgb-registry/srgb) and the
[W3C reference conversion code](https://www.w3.org/TR/css-color-4/#color-conversion-code).
No black normalization, reference-monitor luminance or gamut clamp is included.

- D(u)=u/12.92 for u<=0.04045; otherwise ((u+0.055)/1.055)^(12/5).
- E(u)=12.92*u for u<=0.0031308; otherwise 1.055*u^(5/12)-0.055.

The independently published rounded thresholds are retained. Inverse
composition near the join is not an exact identity.

## BT.709 and BT.2020

Define the project-explicit pair for parameters alpha,beta:

- E(u)=4.5*u for u<beta; otherwise alpha*u^(9/20)-(alpha-1).
- D(u)=u/4.5 for u<4.5*beta; otherwise ((u+alpha-1)/alpha)^(20/9).

The lower segment is open at its upper breakpoint in both directions. The
explicit decode threshold also defines behavior in any small gap/overlap caused
by rounded coefficients. It does not assert a unique inverse in such a gap.

[BT.709-6](https://www.itu.int/rec/R-REC-BT.709) uses alpha=1.099, beta=0.018.
[BT.2020-2 Table 4](https://www.itu.int/rec/R-REC-BT.2020) has three static variants:

| Variant | Definition |
| --- | --- |
| smooth (default) | The unique beta in (0,0.1) satisfying 10*beta^(11/20)=1+(11/2)*beta; alpha=1+(11/2)*beta. This is the standard's continuous-derivative solution. |
| rounded_10bit | alpha=1.099, beta=0.018. |
| rounded_12bit | alpha=1.0993, beta=0.0181. |

The names identify coefficients, not the dtype or code packing of this operator.
BT.709 and BT.2020 rounded_10bit share numbers but retain their distinct selected
transfer descriptions. Both families use the confirmed signed/HDR extension.

## BT.1886

Use the [BT.1886-0 Annex 1](https://www.itu.int/rec/R-REC-BT.1886) EOTF, not
its informative alternative CRT approximation. Required finite Lb,Lw satisfy
0<=Lb<Lw. Let r=5/12, k=Lw^r-Lb^r and b=Lb^r/k, as exact derived quantities.

- D(V)=max(k*V+Lb^r,0)^(12/5).
- E(L)=(L^r-Lb^r)/k.

Semantic V is in [0,1]; semantic L is in [Lb,Lw] cd/m². Raw uses the same
formula; E(0)=-b chooses the boundary representative for D's zero plateau.
Neither exponent nor black/white values are inferred from a display device.

## PQ

Use the EOTF and its specified inverse from
[BT.2100-3 Table 4](https://www.itu.int/rec/R-REC-BT.2100), without the reference
PQ OOTF/OETF composition. Constants are:

m1=2610/16384, m2=2523/32, c1=3424/4096, c2=2413/128, c3=2392/128.

- For z=V^(1/m2), D(V)=10000*(max(z-c1,0)/(c2-c3*z))^(1/m1).
- For y=(L/10000)^m1, E(L)=((c1+c2*y)/(1+c3*y))^m2.

Semantic V is in [0,1]; L is in [0,10000] cd/m². The max term is intrinsic.
E(0)=c1^m2 is positive, not zero. D has a small zero plateau, so D->E does not
recover all near-zero encoded values. Do not replace either behavior with an
identity shortcut. The 10000 scale is fixed; a user peak-luminance parameter
would define a different transform and is not accepted here.

## HLG

Use only [BT.2100-3 Table 5](https://www.itu.int/rec/R-REC-BT.2100) OETF and
inverse OETF. Let a=0.17883277, b=1-4*a, c=1/2-a*ln(4*a). Derived b,c are exact
expressions for the strict reference, not separately rounded decimal constants.

- E(L)=sqrt(3*L) for L<=1/12; otherwise a*ln(12*L-b)+c.
- D(V)=V^2/3 for V<=1/2; otherwise (exp((V-c)/a)+b)/12.

Both semantic input domains are [0,1]. The inverse returns scene-linear values
on the stated normalized scale; this is not an additional output-range clamp.
With the published a, D(1) is approximately 1.00000002693480736626 and E(1) is
approximately 0.99999999506613058251 before rounding. The maintainer selected
these unmodified formulas. Float64 D(1) followed by semantic E fails E's input
domain check. Float32 rounding may hide the excess; its oracle must use its own
dtype. No endpoint snapping or new normalized HLG variant is defined. No system gamma, black lift, display peak, OOTF or complete HLG
EOTF is included. Raw allows formula evaluation outside these sample domains;
it does not switch to an odd extension.

## ACEScc and ACEScct scalar curves

Use the scalar encoding/decoding equations published in the Academy's
[ACEScc](https://docs.acescentral.com/encodings/acescc/) and
[ACEScct](https://docs.acescentral.com/encodings/acescct/) specifications,
inspected 2026-09-23. The exact formulas in this contract identify the versioned
behavior; future website changes do not silently change it. Full ACES color-space
conversions also require the separately specified basis/reference conditions.

Define t=(9.72-15)/17.52 and h=(log2(65504)+9.72)/17.52.

ACEScc encoding, using the first matching branch:

1. L<=0: E(L)=(-16+9.72)/17.52.
2. 0<L<2^-15: E(L)=(log2(2^-16+L/2)+9.72)/17.52.
3. Otherwise: E(L)=(log2(L)+9.72)/17.52.

ACEScc decoding:

1. V<=t: D(V)=2*(2^(17.52*V-9.72)-2^-16).
2. t<V<h: D(V)=2^(17.52*V-9.72).
3. V>=h: D(V)=65504.

For ACEScct let A=10.5402377416545, B=0.0729055341958355,
Xbreak=0.0078125, Ybreak=0.155251141552511, all exact listed decimals.

- E(L)=A*L+B for L<=Xbreak; otherwise (log2(L)+9.72)/17.52.
- D(V)=(V-B)/A for V<=Ybreak; otherwise 2^(17.52*V-9.72) for V<h,
  and 65504 for V>=h.

Do not recompute Ybreak from a rounded multiplication or repair the published
join. Both semantic directions accept finite values, including negative codes
and values above 1, subject to finite output. The ACEScc floor and both decoding
caps are preserved. Cct's negative linear toe is not clipped. No half-float
storage restriction is imposed by the mathematical cap; both dtypes retain it.
