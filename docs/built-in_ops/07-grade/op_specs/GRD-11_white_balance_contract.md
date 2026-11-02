---
spec_schema_version: 1
id: GRD-11
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
---

# GRD-11: White-balance gain and adaptation

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-11A](GRD-11A_white_balance_gains.md) | White balance with RGB gains | `grade.white_balance_gains` |
| [GRD-11B](GRD-11B_white_balance_adapt.md) | White-point adaptation of RGB | `grade.white_balance_adapt` |
| [GRD-11C](GRD-11C_white_balance_temperature_tint.md) | White balance from temperature and tint | `grade.white_balance_temperature_tint` |

## Current parameters

The registered operation keys are the keys in the member table. Raw selection uses `axis: Int64` relative to the cell descriptor, before batch axes, and `components: String` containing three distinct canonical decimal indices separated by commas in RGB order. Described selection uses the serialized FMT `input_description: String` and its `group: String`; roles resolve RGB order and this form excludes raw selectors. The described group must be linear RGB with an explicit relative or absolute reference.

B and C resolve `source_basis: String` from an explicit parameter or the source description. Basis strings are the existing named FMT presets or `basis-v1:` followed by eight comma-separated, sixteen-digit hexadecimal Float64 words for primary xy pairs and white xy. White strings are `d65`, `d50`, `aces`, or `xy-v1:` followed by two such words. An explicit basis assertion must agree with the description. Member pages list adaptation, destination and temperature parameters.

## Shared mathematics and behavior

A applies supplied linear gains. B performs white adaptation in XYZ. C derives the source illuminant white from temperature and tint and performs the same adaptation.

### Temperature and tint model

Use the CIE 1931 2-degree color-matching functions from the [CIE dataset](../research-sources.md#cie-observer), at every integer wavelength from 360 through 830 nm. With wavelength lambda in metres, Planck spectral shape is `P(lambda,T)=lambda^-5/(exp(c2/(lambda*T))-1)`, where c2=h*c/k_B and the SI constants are h=6.62607015e-34 J s, c=299792458 m/s and k_B=1.380649e-23 J/K. Form X,Y,Z by the sums of P times the three tabulated matching functions and normalize Y to one. The common wavelength step and spectral scale cancel.

Convert to CIE 1960 UCS by u=4X/(X+15Y+3Z), v=6Y/(X+15Y+3Z). Differentiate the sampled Planck sums with respect to T to obtain the locus tangent (du/dT,dv/dT). Normalize the perpendicular (-dv/dT,du/dT), changing its sign so its v component is positive. Add source_tint times this unit normal to (u,v). Recover xy with x=3u/(2u-8v+4), y=2v/(2u-8v+4), then W=(x/y,1,(1-x-y)/y). This is the named sampled-Planck native model; tint is a signed UCS distance. It uses the locus/normal interpretation discussed by [Ohno](../research-sources.md#temperature), with the explicit sampling and tangent definition given here.

Publish the normalized source white W to Float64 before adaptation. The resulting white must have positive XYZ and positive adaptation-cone responses; these are caller premises in addition to the temperature/tint intervals. Source temperature describes the illuminant being corrected. The target white is explicit or the working-space white. Adaptation uses this white without an additional exposure normalization.


## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
