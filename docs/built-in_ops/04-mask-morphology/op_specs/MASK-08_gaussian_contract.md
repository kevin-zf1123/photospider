---
spec_schema_version: 1
id: MASK-08
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
---

# MASK-08: Finite Gaussian feather with one specified rounding boundary

Inherits the [MASK baseline](MASK_common_contract.md).

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-08A](MASK-08A_gaussian_feather.md) | gaussian_feather | Finite normalized Gaussian mask feather |

## Interface

Input is Coverage only; the `values` output has the input dtype. Required statics: `sigma_y`,`sigma_x` finite Float64>=0; `radius_y`,`radius_x` Int64>=0; `boundary` zero|replicate|reflect_half.

## Mathematics

Radius is explicit: no hidden truncate=4 or library default rounding of truncate*sigma. A constructor may derive radii externally, but records the chosen integers. There is no fixed semantic radius cap and no image-dimension-derived cap. Static support sizes, coordinate offsets and tap counts use checked arithmetic. If a sigma is zero, that axis has exactly one tap at zero and MUST have radius zero; positive sigma with radius zero is also an identity along that axis.

For active taps define g(dy,dx)=exp(-dy²/(2*sigma_y²)-dx²/(2*sigma_x²)), omitting each zero-sigma term. Z=sum over the entire rectangular support g. Output RN_T(sum g*sample / Z), with exact real exp and ONE final rounding. Coefficients, pass intermediates and Z are not individually rounded to T in the reference. A separable evaluation must reproduce this complete 2D result. g>0 for every finite active tap even where a hardware exp underflows; those taps remain in support. All required input values are finite [0,1] as an unchecked caller premise.

Zero: samples outside D are +0, and the denominator is NOT edge-renormalized. Replicate: map index i to min(max(i,0),n-1). Reflect-half: take j=i mod (2*n) with Euclidean modulo, map j if j<n else 2*n-1-j; for n=1 this always yields 0. This is not whole-sample mirror. The mathematical support is the union of mapped source cells. Radius 0 in both axes copies the input bits without sample validation. Interior constant fields preserve DC; zero-border images do not preserve their constant value at canvas edges.

FFT, IIR and infinite Gaussian are distinct algorithms and are not substituted.

## References

[S10](../research-sources.md#s10); [S11](../research-sources.md#s11)
