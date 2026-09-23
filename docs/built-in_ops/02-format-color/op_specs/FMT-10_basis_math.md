---
spec_schema_version: 1
id: FMT-10-math
kind: mathematical_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-10 exact basis and adaptation mathematics

Inherit [FMT-10](FMT-10_rgb_basis_contract.md). Matrices multiply column vectors;
row i determines output component i. RGB order is R,G,B; XYZ order is X,Y,Z,
independent of physical channel positions. All vectors and whites use the same
CIE XYZ observer convention; this family performs no observer or spectral
conversion. Named presets use CIE 1931 2-degree colorimetry.

## Coordinates and white-normalized RGB basis

Custom coordinates are finite Float64 values, interpreted as exact binary
rationals. Preset literals below expand once with RN64 and are then exact
binary-rational metadata, matching the existing color-description vocabulary.
Do not substitute independently rounded published RGB matrices for the matrix
constructed from those coordinates.

For primary chromaticities (xr,yr),(xg,yg),(xb,yb), define the column matrix P:

```
    [ xr        xg        xb       ]
P = [ yr        yg        yb       ]
    [ 1-xr-yr   1-xg-yg   1-xb-yb  ]
```

For white (xw,yw), require finite xw>0,yw>0 and exact xw+yw<1. Define
W=(xw/yw,1,(1-xw-yw)/yw)^T. Let s=P^-1 W and M=P diag(s).
Both det(P) and every s component must be exactly nonzero, equivalently P and
M are nonsingular. Virtual primaries, primary y=0, negative normalization scales
and exact near-singular bases are allowed. No condition-number cutoff or epsilon
replacement is introduced. Large exact coefficients are not rounded to a finite
machine matrix just to decide validity; resource/precision budgets remain real
limits with explicit errors.

A: XYZ=M RGB. B: RGB=M^-1 XYZ. M(1,1,1)^T=W exactly before sample rounding.
M is dimensionless: relative reference white Y=1 and absolute cd/m²-scaled
coordinates use the same numeric matrix. No Y=100 normalization, white-luminance
ratio, exposure gain or gamut clipping is hidden in A/B.

## Fixed primary/white presets

These literal coordinates match the inspected preset table in
[color_array.cpp](../../../../src/lib/data/color_array.cpp). Preset selection
sets geometry, not transfer or implicit adaptation. A separately named RGB space
must also satisfy its reference/transfer definition.

| Preset | R xy | G xy | B xy | White xy |
| --- | --- | --- | --- | --- |
| srgb_rec709 | (0.64,0.33) | (0.30,0.60) | (0.15,0.06) | (0.3127,0.3290) |
| display_p3 | (0.68,0.32) | (0.265,0.69) | (0.15,0.06) | (0.3127,0.3290) |
| rec2020 | (0.708,0.292) | (0.170,0.797) | (0.131,0.046) | (0.3127,0.3290) |
| adobe_rgb_1998 | (0.64,0.33) | (0.21,0.71) | (0.15,0.06) | (0.3127,0.3290) |
| prophoto_rgb | (0.734699,0.265301) | (0.159597,0.840403) | (0.036598,0.000105) | (0.3457,0.3585) |
| aces_ap0 | (0.73470,0.26530) | (0,1) | (0.00010,-0.077) | (0.32168,0.33767) |
| aces_ap1 | (0.713,0.293) | (0.165,0.830) | (0.128,0.044) | (0.32168,0.33767) |

In particular, keep the current ProPhoto coordinate precision; do not silently
replace it with another rounded table. The ACES white is its specified pair,
not D65 or an arbitrary illuminant derived from a rounded color temperature.
A preset plus contradictory custom coordinates is invalid; customize by giving
the complete coordinate description instead. White shorthand D65/D50/ACES
expands to the listed RN64 pairs. Identity/matching compares resolved numerical
coordinates exactly, with metadata signed-zero canonicalization inherited.

## Full linear white adaptation

Normalize source and target whites Ws,Wt separately to Y=1. For the explicitly
selected response matrix H, form rs=H Ws and rt=H Wt. Every component of both
rs and rt must be strictly positive. Check this exactly before execution in
semantic and raw modes, including equal-white identity requests. Invalid white
responses are parameter errors; input color samples need not have positive
responses or lie inside a gamut.

```
K = inverse(H) * diag(rt[0]/rs[0], rt[1]/rs[1], rt[2]/rs[2]) * H
C(XYZ) = K * XYZ
```

The response matrices below are exact decimal rationals in this contract.
Compute their exact inverses, not a separately rounded inverse table. K Ws=Wt
and K^-1 equals the reversed-white transform in exact arithmetic. Fixed Y=1
white normalization means a common reference-white luminance is retained; K
need not preserve Y for every non-neutral input. No full appearance model or
partial-adaptation degree is implied.

XYZ Scaling: H=I.

Linear Bradford:

```
[  0.8951   0.2664  -0.1614 ]
[ -0.7502   1.7135   0.0367 ]
[  0.0389  -0.0685   1.0296 ]
```

CAT02:

```
[  0.7328   0.4296  -0.1624 ]
[ -0.7036   1.6975   0.0061 ]
[  0.0030   0.0136   0.9834 ]
```

CAT16:

```
[  0.401288   0.650173  -0.051461 ]
[ -0.250268   1.204414   0.045854 ]
[ -0.002079   0.048952   0.953127 ]
```

These names select the specified full linear response-space diagonal transforms.
There is no nonlinear Bradford variant, CAT02 correction variant or automatic
switch between methods. XYZ Scaling remains a genuine selected diagonal matrix
operation; nonidentity zero entries do not prune source dependencies.

## Native rounding and nonfinite behavior

For each requested output row of A/B/C, strict evaluates all three terms plus
an implicit +0 bias using exact rational coefficients and exact input values,
then rounds once to the unchanged output dtype. This follows NUM-14 dot-product
semantics, specialized to statically derived geometry instead of dynamic
machine-valued matrix/bias ports. There is no independently rounded coefficient,
product, matrix-inversion result or hidden cone-response intermediate.

Semantic mode requires all three consumed inputs finite and the requested final
rounded output finite. Negative/HDR values are otherwise allowed. Overflow of
an unused output row is not evaluated or manufactured. Raw applies NUM-14
nonfinite classification: first source NaN in canonical triple order is quieted
with payload/sign preserved; without source NaN, 0*Inf or conflicting signed
infinities produce canonical NaN. An infinite product's sign is the XOR of the exact finite coefficient sign and
the input infinity sign; an exact zero coefficient is canonical +0. A finite exact
zero sum returns +0 due to the implicit +0 bias, and nonzero underflow retains
its sign. Raw final overflow produces the NUM infinity result.

The family's static exact-I bit-identity exception precedes
this dot evaluation and follows that family's copy/validation contract. An
approximate identity, or an identity obtained only by multiplying several
separate node matrices, does not establish that exception.

## Composite D and explicit white handling

D is not a separately once-rounded matrix primitive. In semantic or raw mode,
its requested data flows through the actual A -> optional C -> B expansion:

```
a = RN_dtype(Ms * input)
c = RN_dtype(K * a)       # only for explicit adapt
out = RN_dtype(inverse(Mt) * (c if adapt else a))
```

Every native row evaluation and required sample validation follows its own node
contract, including intermediate overflow and failure. preserve_xyz omits K;
it does not mean that floating A/B rounding disappears. When whites differ,
B's explicit preserve_xyz option changes the destination basis interpretation
without altering XYZ through a hidden adaptation or metadata-only C substitute.

A source-white neutral maps to target-white neutral under adapt in exact math;
it generally becomes non-neutral under preserve_xyz. These are different useful
operations. White luminance, scale, observer and scene/display reference cannot
be changed implicitly by either policy. Opposite round trips are tested against
staged reference evaluation, not bitwise identity.

## Primary sources and conformance limits

- [ICC Technical Note 02-2003](https://www.color.org/chadtag/) supplies the
  linear Bradford matrix and diagonal-ratio construction. Its fixed-point ICC
  chad encoding and its particular white XYZ literals are not our coefficients
  or metadata policy.
- [RIT MCSL Waypoint material](https://www.rit.edu/science/sites/rit.edu.science/files/2019-03/MCSL_Waypoint.pdf),
  equation (5), provides the CAT02 response-space construction.
- Li et al., [A Revision of CIECAM02 and its CAT and UCS](https://library.imaging.org/admin/apis/public/api/ist/website/downloadArticle/cic/24/1/art00035)
  (2016), printed page 210, equation (15), supplies the CAT16 coefficients;
  see also [Comprehensive color solutions: CAM16, CAT16, and CAM16-UCS](https://doi.org/10.1002/col.22131)
  (2017), is the CAT16 model source. This contract uses only the full linear
  adaptation matrix, not the complete model's environmental/nonlinear stages.
- [ACES2065-1](https://docs.acescentral.com/encodings/aces2065-1/) and
  [ACEScg](https://docs.acescentral.com/encodings/acescg/) document virtual AP0/AP1
  primaries. The explicit preset table above fixes the adopted coordinate data.

No commercial product bit compatibility or exact inverse of rounded published
matrix tables is claimed. Independent acceptance uses these exact definitions
and coordinate encodings rather than an external engine with unspecified setup.
