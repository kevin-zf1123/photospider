---
spec_schema_version: 1
id: CRV-06F
parent_id: CRV-06
function: color_ramp_oklch
operation_family: curve.color_ramp_oklch
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-06F: OKLCh ramps

This family uses the polar form of OKLab with fixed D65, not CIELCh(ab).
L uses the OKLab scale (nominal 0..1, all finite extensions allowed), C is finite
and nonnegative, and h has three independent input representations. No alpha,
custom white substitution, RGB conversion or gamut clipping is included.
The [model author](https://bottosson.github.io/posts/oklab/) defines the polar
coordinates by C=sqrt(a*a+b*b), h=atan2(b,a). These relationships identify the
model; the ramp consumes supplied polar coordinates and does not perform this
conversion during interpolation.

## Complete inherited contract

Inherit every port, parameter, default, domain, limit, mathematical, precision,
zero, demand, invalidation, mapping, resource, error and acceptance obligation
from [CIELCh CRV-06D](CRV-06D_color_ramp_cielch.md), replacing the model with OKLCh,
L*/C* with OKLab L/C and configurable white with fixed D65. Specifically:

- Floating radian/pi inputs use colors[K,3]; rational input uses
  lightness_chroma[K,2] and Int64 numerator/denominator arrays, q>0.
- Output dtype defaults to colors or lightness_chroma dtype. Output hue unit
  defaults to radian for radian input and pi_multiple for the other two inputs.
- Hue is the original unnormalized value. Interpolate its raw difference,
  retaining multiple turns. There is no hue_path or normalization parameter.
- C=0 never discards hue. Unit conversion and interpolation are performed as
  one mathematical formula with final correct rounding; all versions match bits.
- Stops are globally validated; selected complete color rows alone are read.
  Any channel request observes the full color, including rational numerator and
  denominator, and carries an output OKLCh/D65/hue-unit description.

Attached descriptions must match the explicit input model and hue representation;
CIELCh descriptors cannot be relabeled as OKLCh. Rational L/C ports alone are not
complete ColorArrays. All shapes, immutable owners, exact dirty support,
cancellation and publication scopes are inherited without relaxation.

## Acceptance and entrypoints

Use stops=[0,1], pi colors=[[0.25,0.125,1.75],[0.75,0.375,0.25]], input=[0.5].
The result is [[0.5,0.25,1]]. Rational hues 7/4 and 1/4 agree. Repeat with C=0:
hue remains 1. With hues 0 and 4, midpoint hue is 2. Reject CIELCh/custom-white
metadata. Use the full CRV-06D independent oracle and execution/resource checks
with the OKLCh description. Public workflow examples remain conceptual until
ColorArray and operations are implemented; actual execution evidence is required.

- [Radian entrypoint](CRV-06F1_color_ramp_oklch.md).
- [Floating pi entrypoint](CRV-06F2_color_ramp_oklch_pi.md).
- [Rational pi entrypoint](CRV-06F3_color_ramp_oklch_rational_pi.md).
- [OKLab counterpart](CRV-06E_color_ramp_oklab.md).
