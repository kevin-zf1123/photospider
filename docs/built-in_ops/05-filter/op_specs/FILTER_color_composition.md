---
spec_schema_version: 1
id: FILTER-color-composition
kind: shared_operator_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
---

# Color, alpha and field composition

Raw field filters do not infer color or alpha from channel count. Semantic entry
points explicitly select a group and its same-tensor alpha slot and respect FMT
canonical straight color. metadata_mode=respect|override follows FMT; override does
not decode transfer functions. Decode, model conversion, gamut mapping, tone mapping
and quantization remain explicit operations.

## Fused positive-weight blur

[FIL-01D](FIL-01D_positive_color_blur.md) is a dedicated primitive for linear RGB or
linear-Y Gray. Its nonnegative Float64 kernel K has finite positive exact sum S.
An associated coverage alpha is finite in [0,1]; absent alpha means coverage 1 and
does not create an alpha output. The public output remains straight. This primitive explicitly overrides the FMT
common finite-color-sample validation rule for participating color values: NaN/Inf
colors are accepted and propagated under NUM. The exception changes neither FMT
metadata/group validation nor the finite [0,1] coverage-alpha domain. It applies
only to FIL-01D, not automatically to other semantic color operations. For each
component and nonzero tap:

```
A_exact = sum(K_i*a_i)/S
P_exact = sum(K_i*a_i*c_i)/S
A_out = RN_t(A_exact)
C_out = NaN(P_exact) ? propagate(P_exact)
      : A_exact > 0 ? RN_t(P_exact/A_exact) : +0
```

Products and quotients have no hidden intermediate dtype rounding. Zero alpha does
not exclude color: read the color at each positive tap, including 0*NaN/Inf and its
NUM propagation. The P reduction uses tap row-major order and written K,a,c operand
order under the numeric contract. All-transparent finite colors yield +0; a
participating invalid product/NaN still propagates. Positive A_exact rounded to zero
retains the corresponding hidden straight color, including NUM exceptional results.
Zero taps are excluded. Alpha-only requests do not read color; color-only requests
read needed color and alpha but do not publish an unrequested alpha plane. Unselected
AOV/emission channels are bit copies, not coverage weighted. Output metadata points
only to its own slots.

This E formula is distinct from the explicit staged FMT associate -> field filter
-> unassociate chain, which has additional rounding boundaries. Box/Gaussian kernel
producers can feed the fused primitive; their coefficient representation remains
explicit and does not imply equivalence to a separately baked separable filter.

## Restoration and other filters

Signed kernels, derivatives, Hessians, FFT and inverse problems produce numerical
fields. Guided-filter equivalent weights can be negative. None inherit the positive
blur alpha formula. Semantic restoration entry points that require opaque color
(including CBM3D, SMAA 1x and dark-channel dehazing) require the selected group to have
no alpha association. Do not scan alpha==1 to admit a group or choose a background.
The caller explicitly extracts straight color or composites onto a chosen background;
replacing color and restoring alpha is an explicit graph. Hidden color retained by
extraction remains algorithm input.

Guide coordinates are independent numerical domains. FMT constructs RGB/Gray/Lab/
OKLab coordinates, and explicit per-component metric scales define distance. Lab
l=L*/100 does not have the same scale as a*/b*. Range/noise sigma has value units;
variance and guided epsilon have squared units. Hue requires a periodic model.

## Masks and acceptance

Application mixing and sample participation are separate. A field blend
RN_t((1-M)*I+M*F) is not automatically coverage compositing. Explicit M=0/1 selection
branches can bit-copy and bypass an unused branch, without suppressing required
upstream errors of other consumers.

Required fixtures include (C,A)=(8,0),(2,1) with equal weights yielding (2,1/2),
finite all-transparent windows yielding +0, zero-alpha NaN/Inf contamination,
first-NaN payload priority, alpha underflow, alpha-only independence and unselected
AOV NaNs. Runtime validation additionally checks metadata identity, group extraction,
output ownership and demand. Conceptual workflows include FMT decode -> select group
-> fused blur -> FMT encode, and model conversion -> extract luminance -> restoration
-> replace -> inverse model. These do not establish commercial preset equivalence.
