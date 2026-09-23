---
spec_schema_version: 1
id: FMT-11
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-11: color-model conversions

This family records the confirmed clarification. Decisions below are
specification requirements, not runtime support. Inherit the
[FMT common contract](FMT_common_contract.md) and the confirmed
[model coverage](FMT_model_conversion_coverage.md). Ordinary numerical behavior
inherits NUM; the [model mathematics](FMT-11_model_math.md) is normative.

## Confirmed member organization

Each conversion direction is independently specified. The maintainer confirmed
this allocation on 2026-09-23:

| Member | Direction |
| --- | --- |
| [FMT-11A](FMT-11A_xyz_to_cielab.md) | XYZ to CIELAB |
| [FMT-11B](FMT-11B_cielab_to_xyz.md) | CIELAB to XYZ |
| [FMT-11C](FMT-11C_cielab_to_cielch.md) | CIELAB to CIELCh(ab) |
| [FMT-11D](FMT-11D_cielch_to_cielab.md) | CIELCh(ab) to CIELAB |
| [FMT-11E](FMT-11E_xyz_to_oklab.md) | XYZ to OKLab |
| [FMT-11F](FMT-11F_oklab_to_xyz.md) | OKLab to XYZ |
| [FMT-11G](FMT-11G_oklab_to_oklch.md) | OKLab to OKLCh |
| [FMT-11H](FMT-11H_oklch_to_oklab.md) | OKLCh to OKLab |
| [FMT-11I](FMT-11I_rgb_to_hsl.md) | RGB to HSL |
| [FMT-11J](FMT-11J_hsl_to_rgb.md) | HSL to RGB |
| [FMT-11K](FMT-11K_rgb_to_hsv.md) | RGB to HSV |
| [FMT-11L](FMT-11L_hsv_to_rgb.md) | HSV to RGB |
| [FMT-11M](FMT-11M_rgb_to_ycbcr_ncl.md) | Encoded RGB to YCbCr |
| [FMT-11N](FMT-11N_ycbcr_ncl_to_rgb.md) | YCbCr to encoded RGB |
| [FMT-11O](FMT-11O_xyz_to_xyy.md) | XYZ to xyY |
| [FMT-11P](FMT-11P_xyy_to_xyz.md) | xyY to XYZ |
| [FMT-11Q](FMT-11Q_color_to_gray.md) | Described XYZ/YCbCr/CIELAB/OKLab to corresponding Gray |
| [FMT-11R](FMT-11R_gray_to_color.md) | Gray to corresponding neutral XYZ/YCbCr/CIELAB/OKLab |
| [FMT-11S](FMT-11S_gray_to_black_white.md) | Gray to binary Black/White, hard-threshold composition |
| [FMT-11T](FMT-11T_black_white_to_gray.md) | Binary Black/White to explicit two-level Gray |

A through R and T are native primitives; S is a compile-time composition. Shared
math/access implementation is permitted but cannot change the observable member
semantics. This table does not itself register operation keys.

## Boundaries already established

- RGB/XYZ basis conversion and white adaptation belong to FMT-10; transfer to
  FMT-09, dtype/code scaling to FMT-06, profile-directed CMYK to FMT-12.
- Conversions compose explicitly. No automatic all-pairs converter, implicit
  transfer, white adaptation, gamut mapping or rendering is introduced.
- Images remain planar with same-size channel planes and canonical straight
  colors. Alpha stays inside the tensor; it is not an external persistent link.
- Metadata describes the values; raw/override follow the common contract.
  Applicable metadata does not certify sample validity.

## Confirmed three-component interface (A through P)

Each call selects one complete ordered three-component group. Input and output
share Float32 or Float64 dtype, shape, channel axis and channel positions.
Target roles map in their model order to the corresponding ordered source
slots; physical slots need not be consecutive. All unrelated groups, internal
alpha and AOV channels copy bit-for-bit. Reordering is explicit through FMT-03.
Integers are unsupported here; semantic code-domain samples require explicit
decoding before use. Raw explicitly selects three distinct ordered components
and applies the formulas to their stored floating values. Gray/binary members
with changing channel counts receive separate contracts.

## Confirmed CIELAB white interpretation

A and B use the explicit reference white in the effective input description.
D50, D65 and valid custom white xy are admitted under the existing exact white
validation. Preserve that white in the output; neither direction performs
chromatic adaptation or silently supplies a missing D50 white. Missing source
interpretation requires assign or explicit call-local override. D65 XYZ to D50
Lab composes FMT-10C before A. Raw white parameters are specified in the static interface below.

A/B accept or produce relative XYZ normalized to reference-white Y=1. Under the
later [shared scale revision](FMT_relative_coordinate_scale.md), native CIELAB
storage is l=L*/100,a*,b*, with reference-white l=1. This supersedes the initial
L*=100 storage choice. Absolute XYZ must
first be explicitly normalized using a chosen reference-white luminance;
0..100 code-domain XYZ must first be decoded. No luminance parameter or implicit
scale change belongs to A/B. Preserve scene/display reference without rendering.

Use the CIE piecewise function and its inverse for finite signed/HDR values.
Negative XYZ uses the low linear branch, not a sign-symmetric cube-root extension.
Do not clip l to 0..1 or restrict finite Cartesian opponent coordinates to a
nominal range. Semantic consumed inputs and requested results must be finite.
Exact point support for A is l<-Y, a*<-X,Y, b*<-Y,Z; for B it is X<-l,a*,
Y<-l, Z<-l,b*. Validation follows those sets; unrelated components are not read.

The [CIE definition](https://cie.co.at/eilvterm/17-23-076) provides the forward
piecewise equations. The inverse and extended-domain contract are specified
explicitly by this family; mathematical extension is not a claim about physical
realizability or perceptual accuracy of out-of-nominal coordinates.

## Confirmed Cartesian/polar color pairs

C/G generate hue from atan2(b,a) on its signed principal branch [-pi,pi].
Output hue unit is explicitly radian or pi_multiple. If both a and b are
exact zeros, hue is canonical +0 regardless of input zero signs; there is no
near-achromatic epsilon. For negative a with b=+0/-0, hue is +pi/-pi respectively.
No one-turn canonical storage restriction is imposed on input color descriptions.

D/H accept arbitrary finite winding-bearing hue in the declared unit, using
periodic trigonometry without mutating that stored input. Cartesian coordinates
cannot retain winding count or achromatic source hue. A round trip through a/b
therefore regenerates principal hue rather than recovering those lost values.

D/H semantic requests for a or b read and validate both C and hue, even when
C=0. Require finite C>=0 and finite hue; zero C produces canonical +0 in each
requested Cartesian opponent component. Positive C follows C*cos(h) or C*sin(h).
Raw admits negative C under its numerical formula. L-only requests in either
direction read only L, copy its bits and, in semantic mode, check its finiteness.
The semantic zero-chroma shortcut does not waive hue validation.

## Confirmed OKLab coefficient definition

E/F fix the two forward XYZ-to-LMS and LMS-to-OKLab matrices in
[CSS Color 4, 2026-09-13](https://www.w3.org/TR/2026/CRD-css-color-4-20260913/#color-conversion-code).
Their printed decimal coefficients are exact decimal rationals. F uses exact
inverses of those same two matrices, not independently rounded inverse tables.
NUM strict/accelerated arithmetic applies; JavaScript statement rounding is not
the reference. Retain any actual white-point residual without an added neutral
correction. This is a fixed mathematical definition, not a browser-compatibility
claim or an automatic choice among coefficient versions.

E/F require relative XYZ with reference-white Y=1 and D65 xy=(0.3127,0.3290),
resolved using the existing RN64 geometry convention. Other whites need explicit
FMT-10C adaptation and absolute values explicit normalization before E. Preserve
scene/display reference. Use the real signed cube root of LMS and its cubic
inverse for all finite extended coordinates; do not clip OKLab L to 0..1 or
restrict finite opponent coordinates. Every requested transformed component
reads and validates the complete source triple in semantic mode. Intermediate
LMS is mathematical computation, not a separately published rounded tensor.

## Confirmed HSL/HSV source interpretation

I/J/K/L operate on explicitly described relative RGB coordinates, either linear
or transfer-encoded. Preserve their underlying primaries, white, transfer and
scene/display reference in the HSL/HSV description and restore them on conversion
back to RGB. No transfer decoding or encoding occurs within these members.
HSL derived from linear RGB and HSL derived from encoded RGB are distinct
interpretations. HSB is an authoring alias for this family's HSV, canonicalized
to the single serialized model HSV; it introduces no separate mathematics.

Use an explicit algebraic extension of the max/min formulas without clipping
finite negative/HDR RGB or the resulting S,L,V. With M=max(R,G,B), m=min(R,G,B),
chroma=M-m: HSV has V=M and S=chroma/V; HSL has L=(M+m)/2 and
S=chroma/(1-abs(2*L-1)). Exact chroma=0 selects H=+0,S=+0. For non-gray input,
a zero denominator makes the requested saturation invalid in semantic mode;
raw retains NUM division semantics. In particular RGB=(1,0,-1) has a singular
HSL saturation and RGB=(0,-1,-2) a singular HSV saturation. Exact request scope
for these singularities is specified with the member dependency contract.

I/K generate six-sector hue in the mathematical range [0,2*pi), or [0,2) for
pi_multiple, with explicitly selected output unit. Gray generates +0. Final
floating rounding remains NUM rounding, not an extra wrap of a value rounded
to the upper endpoint. J/L accept arbitrary finite multi-turn hue and use
periodic evaluation without rewriting input metadata/samples. Every requested
inverse RGB component reads and semantically validates the full H,S,L/V triple,
including S=0 or V=0; do not suppress invalid input through an achromatic shortcut.

## Confirmed YCbCr scope

M/N cover non-constant-luminance (NCL) coordinates only: BT.601, BT.709,
BT.2020 NCL or explicit custom Kr,Kb under the existing positive-coefficient
rules (Kr>0, Kb>0, Kr+Kb<1). Use native floating Y', zero-centered Cb/Cr,
nominally [0,1],[-0.5,0.5],[-0.5,0.5], while admitting finite signed/HDR
extensions without clipping. The matrix preset never chooses RGB primaries or
transfer. Preserve that separate source interpretation. FMT-06 owns integer
full/limited-range encoding, and codecs own external chroma subsampling.
BT.2020 constant luminance requires a separate future member; it is not an M/N
option or an implicit nonlinear stage in an otherwise NCL matrix transform.

M/N semantic interpretation requires a declared encoded RGB basis, not linear
RGB; raw can apply the matrix directly to numerical components. M requests for
any transformed output read all R',G',B'. N requests for R' read Y',Cr; B' read
Y',Cb; G' read all Y',Cb,Cr. Semantic sample validation follows exactly those
sets. Thus invalid Cb does not fail an R'-only request in N.

## Confirmed XYZ/xyY singularities

O preserves Y by an independent bit copy, validating only Y when that is the
sole requested transformed output. x/y require X,Y,Z and exact sum T=X+Y+Z.
For semantic exact black (all three equal zero, irrespective of zero signs),
return the effective reference-white x/y. A nonzero triple with T=0 fails a
requested x/y in semantic mode; no epsilon or fallback chromaticity is used.
Other finite inputs use X/T,Y/T without physical-triangle clipping. These sample
chromaticities are distinct from the strictly positive reference-white parameter.

P computes X=x*Y/y and Z=(1-x-y)*Y/y. Requested X/Z require finite x,y,Y and
nonzero y in semantic mode even when Y=0. No guessed black shortcut is present.
Y-only output independently copies and validates Y without reading x/y.
O/P preserve relative or absolute cd/m² Y scale, observer, white and scene/display
reference; they do not normalize luminance. Finite signed/HDR coordinates remain
allowed outside the explicit singularities. O's semantic black fallback has
positive mathematical reference-white y; reconstruction through P also requires
the actual rounded stored y to remain nonzero. Very small legal whites may
underflow in Float32 and lose that property; do not add an epsilon or hidden clamp.

## Confirmed Gray meanings

Admit four distinct Gray interpretations: linear luminance Y, encoded-RGB
weighted luma Y', normalized CIELAB l=L*/100 and OKLab L. Metadata explicitly distinguishes their
units, reference/white and applicable underlying RGB/matrix/transfer. A bare
Gray label is insufficient. Reconstruction yields a neutral color under the
corresponding description, never recovered source chroma. CIELAB l=0.5 is not
linear RGB=(0.5,0.5,0.5). Arbitrary artistic channel weights remain GRD-12.

Q accepts XYZ/YCbCr/CIELAB/OKLab and selects respectively Y/Y'/l/L into the
corresponding complete Gray description. R reconstructs respectively Y*W with
W normalized to Y=1, (Y',0,0), (l,0,0) or (L,0,0). Q/R do not accept arbitrary
RGB and silently choose a path. RGB source and destination workflows explicitly
compose FMT-10 and the appropriate A/B, E/F or M/N members. Transfer, white
adaptation and any absolute/relative normalization stay explicit. Reconstruction
uses the source Gray's interpretation, not an automatically selected new white.

Q replaces the selected triple at its smallest physical channel index with one
Gray channel and removes its other two slots. Other channels retain relative
order and remap their descriptions/references. The channel axis remains even
when only one channel survives. R replaces the Gray slot with three consecutive
target components in canonical model order. Other channels shift and remain
unchanged, including internal alpha. An independent Gray tensor without a
channel axis requires an explicit axis insertion for R. Other output orderings
use FMT-03. Shape inference is static; Q reduces channel count by two and R
increases it by two (or inserts size three for axis-free Gray).

Q reads and semantically validates only the extracted coordinate, not discarded
chroma. R requests read Gray only where the requested formula depends on it;
new constant-zero opponent/chroma components have no Gray sample dependency.
Actual Gray reads still validate finite semantic coordinates regardless of alpha.
Thus invalid Lab a*/b* does not prevent extraction of finite l, and requesting
only R's new Lab a*=0 does not read or validate its Gray l source sample.

## Confirmed Black/White members

S is an explicit composition using the MASK-03 hard threshold x>=threshold.
Threshold is required in the source Gray's native unit. Produce canonical 0/1
Black/White samples and retain the source Gray interpretation as provenance;
binary appearance is not an alpha/coverage declaration. T selects an explicitly
provided black or white Gray value per sample, without recovering original tone.
Dither/halftone remain GRD-29/30, integer encoding FMT-06 and bit packing codecs.

S/T preserve Float32/Float64 dtype, shape and slots, replacing only the selected
Gray/binary component and copying all unrelated channels. T requires exact 0 or
1 input in every mode, with -0 selecting black. All other numbers, NaN and Inf
fail as invalid two-level selectors; raw does not reinterpret nonzero as white.
The required black_value and white_value are finite same-dtype typed constants;
they may coincide or be reversed. Select their exact stored bits without
interpolation. Integer code-domain binary data must first be explicitly decoded.

Current mask.threshold is a typed Float32 Whole implementation. It is not already
a conforming generic/Float64/exact-region building block for S. Implementing S
requires an admitted MASK-03 primitive with the requested target contract (or a
formally equivalent composition), not a silent switch to Whole or a claimed
working helper over the retired representation.

## Static interface and interpretation

Each native member has one required tensor Value port `input` and one `values`
output. S takes an input edge and yields one edge after static composition.
No runtime parameter streams or Result schemas are introduced. Dtype remains
Float32/Float64. Generic ranks/extents/counts and checked arithmetic inherit NUM,
with the kernel's additional actual image representation limits.

| Static logical field | Requirement |
| --- | --- |
| metadata_mode | respect default; override or raw explicit |
| group | One uniquely selected complete semantic source group; forbidden with raw selectors |
| axis/components | Raw structural channel axis and ordered distinct triple for A-Q; single component for R/S/T, or explicit axis-free scalar-field structure |
| metadata_override | Effective source description only in override; cannot change physical layout |
| source assertions | Optional source descriptions must match effective metadata |
| white | A/B resolve source white semantically; raw A/B require explicit valid xy. E/F semantic white is fixed D65. O semantic black uses source white; raw O performs ratios without a white parameter |
| output_hue_unit | Required radian or pi_multiple for C/G/I/K; no default |
| input_hue_unit | D/H/J/L resolve from semantic source; raw requires it explicitly |
| ncl_matrix | M requires preset or Kr/Kb; N resolves source metadata, optional assertion must match; raw M/N require explicit coefficients/preset |
| gray_kind | Q resolves from source model; R from Gray; raw Q/R require linear_y, encoded_luma, cielab_l or oklab_l |
| gray_white | R linear_y resolves Gray white semantically; raw requires valid xy |
| output_axis | Required only for axis-free R insertion; otherwise preserve existing axis |
| threshold | S required finite Float64, no default, in native Gray units |
| black_value/white_value | T required finite typed constants matching input dtype, no defaults |
| layout | auto default, view or materialize |

Reject unknown, contradictory and inapplicable fields before samples, including
Empty/bypass-only requests. Parameters, roles, units and output mappings are
static; changes require reinference/recompilation. Native CIE lightness is l.
Semantic non-native codes must be decoded explicitly; raw uses stored floats.
R consumes native Gray, so transfer-encoded linear-Y Gray first needs explicit
decoding. Luma's underlying RGB transfer does not imply Y' is the transfer of
the original color's Y. T semantic output uses its binary input's declared Gray
origin; an undescribed binary source first needs assign/override.

Semantic output rebuilds the selected group and preserves applicable white,
reference, observer, coordinates, units and remapped internal alpha relations.
No observer/spectral conversion occurs. Another group's conflicting references
to edited/deleted channels cause a static error; do not silently drop that group.
Raw keeps applicable unverified descriptions without claiming target conversion;
on Q/R remove structurally inapplicable complete groups rather than labeling an
incomplete/expanded tuple as the old complete group. Inputs and other consumers
remain immutable. No premultiplied semantic input is admitted.

## Numerical and special-value contract

Strict rounds each complete requested formula once, apart from specified copies
or selections. No rounded intermediate matrix, l*100, root, luma or hue conversion
is inserted. Finite branch predicates and static validity are exact. Accelerated
finite results inherit NUM's final FP32-scaled bound, including Float64; copies,
selections, classification and errors remain exact. The math supplement defines
raw NaN/Inf and zero behavior. Semantic validation follows actual component
support and requires finite consumed values and requested results. Alpha is
never read and alpha=0 never suppresses actual color work. Intermediate machine
overflow cannot fail a representable final result. Negative/HDR samples remain
legal except the explicit chroma, singularity and binary-selector rules.

Raw does not apply semantic black or zero-chroma substitutions. Static structural
and parameter legality, plus T's intrinsic binary selector rule, still apply.
Copied samples retain all bits, including signaling NaN when no semantic check
rejects them. Computed exact cancellation is +0; specified trigonometric/atan2
and copy rules take precedence. Nonzero underflow retains its sign.

## Exact demand, layout, dirty mapping and publication

Demand is the union of the same-position component sets listed in the math and
member files. No halo, neighborhood or Whole fallback occurs. Dirty propagation
is their forward relation restricted to observed outputs. Constant R zero
outputs have no sample dependency. All descriptors still undergo static checks;
Empty reads no payload. Bypass alpha/AOV/other groups read only corresponding
source samples. Required upstream Whole computations retain their own scope.

Q may return a legal same-owner read-only view of its bit-copy mapping, while
still performing semantic validation. auto materializes when that view cannot
be represented; forced view then fails. No whole-node static identity is admitted
for A-P/R/S/T; those arithmetic members materialize and reject forced view,
even if the particular request selects only copied lightness/Y or bypass alpha.
materialize forces new storage and computes only requested coverage. There is
no cross-owner view, source mutation or implicit complete-image scan.

Images obey mandatory planar storage, DAG-wide tile geometry, edge-row padding,
page-aligned tile starts and a full-image virtual reservation. Kernel preparation
and retained windows cover only required pages; page availability does not
authorize reading padding or unrelated channels. Publish exact coverage at
global coordinates; gaps are not zero pixels. Owners and immutable metadata
survive context teardown while referenced. Failed publication units remain
unavailable; already valid independent regions follow kernel lifetime/failure
rules. No eviction, private page-fault evaluation or hidden CPU pool is added.

S expands semantic finite-Gray validation, exact hard comparison, typed 0/1
selection and binary metadata publication, with bypass channels. Its admitted
constituents must preserve support, bits, validation and failure; raw comparison
requires the math supplement's NUM semantics. These are future MASK-03/graph
requirements, not properties of the current Whole threshold. No new universal
model-convert dispatcher or unsuffixed alias is introduced.

## CPU algorithm, resources and cancellation

Native members specify strict, accelerated_apple_silicon and accelerated_x86_64
CPU profiles. S uses conforming constituent profiles and has no native key. No
GPU implementation is promised. Unsupported platform keys fail BackendUnavailable.
Reference evaluation traverses requested samples through kernel windows,
validates actual support, computes the formula and publishes valid coverage.
Exact rational/directed-interval refinement establishes strict root, atan2 and
trigonometric rounding; ordinary libm or Float64 intermediates alone do not prove
conformance. Vectorization may share work but cannot change reads or failures.

For Nq requested and Dq distinct demanded samples, mapping/fixed arithmetic is
O(Nq+Dq), plus actual refinement and metadata/region traversal. Budget fixed
matrices, exact limbs/interval scratch, mappings, page/window sets, output pages,
source ancestry and S intermediates. Distinguish virtual reservation, committed
backing, managed peak and RSS. Sparse requests do not commit or scan all pixels.
Optional sample caching starts off; no mutable process-global math cache.
Poll cancellation before preparation/publication, at least every 1024 visited
sample positions and inside bounded exact-refinement/metadata loops. Charge
fallback work; exhausted proof/work/capacity fails ResourceExhausted, never
reduced-precision success. Release scratch/windows on all exits and published
owners at final retirement. Preserve upstream/cancellation errors.

## Errors and prerequisites

| Trigger | Phase / category |
| --- | --- |
| Invalid/missing parameters, selector, unit, white/NCL data, insertion axis or conflicting metadata | Compile/direct preflight: InvalidArgument / InvalidDomain |
| Unsupported dtype/model/scale/reference, description mismatch or premultiplied semantic source | Compile/direct preflight: TypeMismatch |
| Forced view for arithmetic member or impossible Q view | Static layout admission failure under the shared FMT layout contract |
| Nonfinite semantic support, negative semantic chroma, requested singular S or xyY ratio, illegal T selector | Evaluation: OperationFailed / InvalidDomain, requested publication scope and coordinate/component attribution |
| Rounded nonfinite result from finite semantic arithmetic | Evaluation: OperationFailed / ArithmeticOverflow |
| Capacity/work/shape overflow, cancellation, stale or upstream failure | Preserve NUM/kernel category, scope and origin |

Implementation needs explicit generic model/unit metadata, normalized CIE
lightness schema, group remapping, exact component requests and admitted S
constituents. Legacy ColorArray v1 is not silently relabeled. Member workflows
are conceptual until public implementations/helpers are registered.

## Acceptance and remaining boundaries

Inherit independent formula fixtures and each member's workflow obligations.
Cover both dtypes/profiles, noncanonical role slots, offset/disjoint/cross-tile
regions, selected versus unselected NaN, hidden colors, bypass/Empty, signed
zeros, threshold neighbors, extreme finite values, raw special values, metadata
conflicts, Q/R structural mapping, budgets/cancellation, cache-off and lifetime.
Do not substitute a bitwise round-trip promise for a lossy conversion or rounded
composition. Report actual execution separately from these future obligations.
Sources are pinned with each formula. Physical validity outside nominal domains,
browser bit identity, device CMYK, future BT.2020 CL and complete rendering are
not claimed. No operator-local interview question remains; schema/runtime work
and the separate future CL member remain explicit dependencies.
