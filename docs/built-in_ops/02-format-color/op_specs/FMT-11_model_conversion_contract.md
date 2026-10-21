---
spec_schema_version: 1
id: FMT-11
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
---

# FMT-11: color-model conversions

The family specification remains Proposed. Native members are registered as fixed invocation ABI 8 CPU operations; S is a public helper that lowers to the MASK threshold operation. Inherit the
[FMT common contract](FMT_common_contract.md) and the confirmed
[model coverage](FMT_model_conversion_coverage.md). Ordinary numerical behavior
inherits NUM; the [model mathematics](FMT-11_model_math.md) is normative.

## Confirmed member organization

Each conversion direction is specified independently:

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
semantics. Members A-R and T each have strict, Apple Silicon and x86-64 CPU keys. S is an installed graph helper that appends `mask.threshold_channel_<profile>`; it does not register a color-model key.

## Boundaries already established

- RGB/XYZ basis conversion and white adaptation belong to FMT-10; transfer to
  FMT-09, dtype/code scaling to FMT-06, profile-directed CMYK to FMT-12.
- Conversions compose explicitly. No automatic all-pairs converter, implicit
  transfer, white adaptation, gamut mapping or rendering is introduced.
- Color channels retain a shared logical sample shape and canonical straight-color
  meaning. Generic and spatial tensor storage are supported. Alpha stays inside
  the tensor; it is not an external persistent link.
- `input_description` is explicit static configuration. Without it, callers
  provide raw selectors and member-specific geometry. Results carry no
  description; supplied configuration does not certify sample validity.

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
validation. The result is interpreted on that white; callers supply it downstream explicitly, and neither direction performs
chromatic adaptation or silently supplies a missing D50 white. Missing source
interpretation requires an explicit input_description. D65 XYZ to D50
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
Y<-l, Z<-l,b*. Caller numerical preconditions follow those sets over complete retained output tiles.

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

D/H semantic requests for a or b read both C and hue, even when
C=0. Require finite C>=0 and finite hue; zero C produces canonical +0 in each
requested Cartesian opponent component. Positive C follows C*cos(h) or C*sin(h).
Raw admits negative C under its numerical formula. L-only requests in either
direction read only L and copy its bits. Semantic finiteness is a caller precondition, including hue at zero chroma.

## Confirmed OKLab coefficient definition

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
reads the complete source triple; semantic finiteness is a caller precondition. Intermediate
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
a zero denominator violates the caller precondition for semantic saturation;
raw retains NUM division semantics. In particular RGB=(1,0,-1) has a singular
HSL saturation and RGB=(0,-1,-2) a singular HSV saturation. Exact request scope
for these singularities is specified with the member dependency contract.

I/K generate six-sector hue in the mathematical range [0,2*pi), or [0,2) for
pi_multiple, with explicitly selected output unit. Gray generates +0. Final
floating rounding remains NUM rounding, not an extra wrap of a value rounded
to the upper endpoint. J/L accept arbitrary finite multi-turn hue and use
periodic evaluation without rewriting input metadata/samples. Every requested
inverse RGB component reads the full H,S,L/V triple. Its semantic validity is a caller precondition, including S=0 or V=0.

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
Y',Cb; G' read all Y',Cb,Cr. Semantic sample preconditions follow those sets. Complete output tiles determine which formulas execute and therefore which supports apply.

## Confirmed XYZ/xyY singularities

O preserves Y by an independent bit copy; its semantic precondition concerns only Y when no other transformed component is retained. x/y require X,Y,Z and exact sum T=X+Y+Z.
For semantic exact black (all three equal zero, irrespective of zero signs),
return the effective reference-white x/y. A nonzero triple with T=0 is outside the semantic precondition for x/y; no epsilon or fallback chromaticity is used.
Other finite inputs use X/T,Y/T without physical-triangle clipping. These sample
chromaticities are distinct from the strictly positive reference-white parameter.

P computes X=x*Y/y and Z=(1-x-y)*Y/y. Requested X/Z require finite x,y,Y and
nonzero y in semantic mode even when Y=0. No guessed black shortcut is present.
Y-only output independently copies Y without reading x/y.
O/P preserve relative or absolute cd/m² Y scale, observer, white and scene/display
reference; they do not normalize luminance. Finite signed/HDR coordinates remain
allowed outside the explicit singularities. O's semantic black fallback has
positive mathematical reference-white y; reconstruction through P also requires
the actual rounded stored y to remain nonzero. Very small legal whites may
underflow in Float32 and lose that property; do not add an epsilon or hidden clamp.

## Confirmed Gray meanings

Admit four distinct Gray interpretations: linear luminance Y, encoded-RGB
weighted luma Y', normalized CIELAB l=L*/100 and OKLab L. Explicit configuration distinguishes their
units, reference/white and applicable underlying RGB/matrix/transfer. A bare
Gray label is insufficient. Reconstruction yields a neutral color under the
corresponding description, never recovered source chroma. CIELAB l=0.5 is not
linear RGB=(0.5,0.5,0.5). Arbitrary artistic channel weights remain GRD-12.

Q accepts XYZ/YCbCr/CIELAB/OKLab and selects respectively Y/Y'/l/L into the
corresponding Gray coordinate. R reconstructs respectively Y*W with
W normalized to Y=1, (Y',0,0), (l,0,0) or (L,0,0). Q/R do not accept arbitrary
RGB and silently choose a path. RGB source and destination workflows explicitly
compose FMT-10 and the appropriate A/B, E/F or M/N members. Transfer, white
adaptation and any absolute/relative normalization stay explicit. Reconstruction
uses the source Gray's interpretation, not an automatically selected new white.

Q replaces the selected triple at its smallest physical channel index with one
Gray channel and removes its other two slots. Other channels retain relative
order; callers remap any downstream descriptions explicitly. The channel axis remains even
when only one channel survives. R replaces the Gray slot with three consecutive
target components in canonical model order. Other channels shift and remain
unchanged, including internal alpha. An independent Gray tensor without a
channel axis requires an explicit axis insertion for R. Other output orderings
use FMT-03. Shape inference is static; Q reduces channel count by two and R
increases it by two (or inserts size three for axis-free Gray).

Q reads only the extracted coordinate, not discarded chroma. R requests read Gray only where the requested formula depends on it;
new constant-zero opponent/chroma components have no Gray sample dependency.
Actual Gray reads require finite semantic coordinates as a caller precondition, regardless of alpha.
Thus invalid Lab a*/b* does not prevent extraction of finite l, and requesting
only R's new Lab a*=0 has no Gray sample dependency. Tile closure may retain other output components with their own dependencies.

## Confirmed Black/White members

S is an explicit composition using the MASK-03 hard threshold x>=threshold.
Threshold is required in the source Gray's native unit. Produce canonical 0/1
Black/White samples; callers retain the source Gray interpretation in their configuration;
binary appearance is not an alpha/coverage declaration. T selects an explicitly
provided black or white Gray value per sample, without recovering original tone.
Dither/halftone remain GRD-29/30, integer encoding FMT-06 and bit packing codecs.

S/T preserve Float32/Float64 dtype, shape and slots, replacing only the selected
Gray/binary component and copying all unrelated channels. T requires exact 0 or
1 input as a caller precondition in every mode, with -0 selecting black. Other numbers, NaN and Inf lie outside this contract; no runtime selector validation is performed.
The required black_value and white_value are finite same-dtype typed constants;
they may coincide or be reversed. Select their exact stored bits without
interpolation. Integer code-domain binary data must first be explicitly decoded.

S is implemented by the registered `mask.threshold_channel_<profile>` operation. The public FMT-11S helper serializes its statics and appends that operation; normal compilation validates the resulting graph.

## Static interface and interpretation

Each native member accepts one Result containing one tensor , then publishes one `values` Result containing one tensor. The tensor dtype is Float32 or Float64. Input and output full sample rank, including batch and cell axes, is at most 8. Sample counts follow Result schema representability and execution resource limits. The source channel extent is at most 65536; axis-free R insertion also requires room within the rank limit. Cell-axis parameters exclude the batch prefix. Every connected input undergoes static schema checking. Each complete retained output tile maps its formula supports to input regions and closes them through the source tile grid. A task with only source-independent constants retains a schema dependency without payload reads. Empty selection creates no payload task. Semantic numerical validity and T's exact binary selector rule are caller preconditions, not runtime validation requests.

| Static logical field | Requirement |
| --- | --- |
| input_description | Optional explicit TensorDescription; absence selects the raw numerical path. |
| group | Described path: optional selector when exactly one complete matching group exists; ambiguity fails. |
| axis/components | Raw ordered distinct triple for A–Q, single component for R/S/T, or axis_free=true for R/S/T. In the described path these may assert the configuration's exact axis and indices. |
| white_x/white_y | Raw A/B/O require explicit valid xy. Described A/B/O use input_description white and reject these raw fields. E/F described white is fixed D65. |
| output_hue_unit | Required radian or pi_multiple for C/G/I/K; no default |
| input_hue_unit | D/H/J/L resolve from semantic source; raw requires it explicitly |
| ncl_matrix | M requires preset or Kr/Kb; N resolves source metadata, optional assertion must match; raw M/N require explicit coefficients/preset |
| gray_kind | Q resolves from source model; R from Gray; raw Q/R require linear_y, encoded_luma, cielab_l or oklab_l |
| gray_white_x/gray_white_y | R linear_y resolves Gray white semantically; raw requires valid xy |
| output_axis | Required only for axis-free R insertion; otherwise preserve existing axis |
| threshold | S required finite Float64, no default, in native Gray units |
| black_value/white_value | T required finite typed constants matching input dtype, no defaults |
| layout | auto default or materialize produce independent tiles; only Q accepts view statically, and a nonempty view task fails ViewUnavailable. |
| algorithm | auto default, scalar or reference. |

Reject unknown, contradictory and inapplicable fields before samples, including
Empty/bypass-only requests. Parameters, roles, units and output mappings are
static; changes require reinference/recompilation. Native CIE lightness is l.
Semantic non-native codes must be decoded explicitly; raw uses stored floats.
R consumes native Gray, so transfer-encoded linear-Y Gray first needs explicit
decoding. Luma's underlying RGB transfer does not imply Y' is the transfer of
the original color's Y. T semantic output uses its binary input's declared Gray
origin; the caller supplies its Gray origin explicitly in input_description.

Outputs preserve source schema id, tensor key and batch prefix and set
`atomic_trailing_axes` to zero. They contain structure and samples only. The
caller supplies the resulting model, white, reference, units and remapped alpha
indices to later consumers. No observer or spectral conversion occurs.
Conflicting overlapping groups in the explicit input configuration are rejected
where the transform would invalidate their interpretation; no hidden label
repair is performed. Inputs and other consumers remain immutable. Described
color conversion does not admit a premultiplied source interpretation.

## Numerical and special-value contract

Strict rounds each complete requested formula once, apart from specified copies
or selections. No rounded intermediate matrix, l*100, root, luma or hue conversion
is inserted. Finite branch predicates and static validity are exact. Accelerated
finite results inherit NUM's final FP32-scaled bound, including Float64; copies,
selections, classification and errors remain exact. The math supplement defines
raw NaN/Inf and zero behavior. Semantic validity follows actual component support over complete retained output tiles and requires finite consumed values and results as caller preconditions. Converted formulas do not consume alpha, while retained bypass alpha is read and copied. Alpha=0 never suppresses actual color work. Intermediate machine
overflow cannot fail a representable final result. Negative/HDR samples remain
legal except the explicit chroma, singularity and binary-selector rules.

Raw does not apply semantic black or zero-chroma substitutions. Static structural
and parameter legality, plus T's intrinsic binary selector rule, still apply.
Copied samples retain all bits, including signaling NaN; semantic callers remain responsible for their numerical preconditions. Computed exact cancellation is +0; specified trigonometric/atan2
and copy rules take precedence. Nonzero underflow retains its sign.

## Fixed tile dependencies, layout and ownership

Each selected complete output tile lowers to one CPU Host task. Lowering intersects that tile with the immutable channel spans, maps each formula's source coordinates and unions tiles on the source's own grid. Disjoint selected channels do not introduce a bounding-box dependency across unused channels. Source tiles can contain extra samples, and output tile closure can retain additional formulas; the formula support is evaluated for every retained output sample. Bypass slots copy their corresponding source, while constant R opponents retain only schema dependencies when no other output in the task reads payload.

Auto/materialize write independent dense output allocations. Q accepts forced view as a static option, but every nonempty view task has schema-only input dependencies, zero scratch and returns InvalidArgument/ViewUnavailable. Other members reject forced view statically, including bypass-only selections. Empty creates no tasks while retaining all static parameter and schema checks. Generic and spatial layouts use the same point mappings; input grids respect their own batch and atomic axes. Source owners are borrowed through task completion, and published outputs retain only their own allocations.

S expands to the registered MASK channel-threshold operation with its selected CPU profile. No unsuffixed model-conversion alias is registered.

## CPU arithmetic, resources and cancellation

The profiles are strict, accelerated_apple_silicon and accelerated_x86_64. Unsupported platform profiles fail BackendUnavailable; this family provides no GPU implementation. Kernel preparation checks the exact immutable read slots, output tile, dtype, Host placement, empty task-parameter payload and scratch bound before returning the fixed entry point. Expected source closure is bounded before enumeration, including malformed preparations with too few supplied reads.

Pure copy and constant tasks declare zero scratch. Arithmetic tasks reserve one fixed ModelMath workspace plus a 64-point gather/candidate block. The workspace owns bounded rational limbs and one checked interval pool shared by roots, powers, trigonometry and atan2. Strict rounds the complete formula; auto/scalar retain independently checked fast candidates and their specified SIMD policy. Scalar classification preserves raw IEEE behavior and semantic mathematical special cases, including the configured black-XYZ and zero-chroma substitutions. Numerical preconditions are not scanned or certified at runtime.

Precision retry is distinct from cancellation and capacity failure. The first operational error is retained, and no work callback runs after it. Invocation arithmetic and its error-state transitions allocate no heap storage; the work callback borrows the invocation for its synchronous lifetime. Each run has separate scratch, and immutable prepared kernels may be reused concurrently. Cancellation is checked before work, between at most 64 copied/gathered samples and inside exact arithmetic loops. The floating environment is restored on return. The executor publishes successful calls in order and drains accepted tasks before releasing their borrowed inputs. Per-operation numeric diagnostic reports are not emitted by this fixed ABI path.

## Errors and prerequisites

| Trigger | Phase / category |
| --- | --- |
| Invalid/missing parameters, selector, unit, white/NCL data, insertion axis or conflicting metadata | Compile/direct preflight: InvalidArgument / InvalidDomain |
| Unsupported dtype/model/scale/reference, description mismatch or premultiplied semantic source | Compile/direct preflight: TypeMismatch |
| Forced view for a member other than Q | Compile/direct preflight: InvalidArgument / InvalidDomain |
| Nonempty Q forced view | Fixed task: InvalidArgument / ViewUnavailable; auto/materialize allocate independent outputs |
| Nonfinite semantic support, negative semantic chroma, singular semantic saturation/xyY ratio, illegal T selector | Outside caller numerical preconditions; no runtime validation guarantee |
| Rounded nonfinite result from finite semantic arithmetic | Outside caller numerical preconditions |
| Capacity/work/shape overflow, cancellation, stale or upstream failure | Preserve NUM/kernel category, scope and origin |

The implementation uses explicit model and unit metadata, normalized CIE
lightness, group remapping, exact component requests and the registered S
constituent. NUM/CRV tuple consumers now use the same native units and description
codec; the old ColorArray v1 decoder is removed. Public helpers are
available from the installed `photospider/ops/format/model_conversion.hpp` header.

## Acceptance and remaining boundaries

Inherit independent formula fixtures and each member's workflow obligations.
Cover both dtypes/profiles, noncanonical role slots, offset/disjoint/cross-tile
regions, selected versus unselected NaN, hidden colors, bypass/Empty, signed
zeros, threshold neighbors, extreme finite values, raw special values, metadata
conflicts, Q/R structural mapping, budgets/cancellation and lifetime.
Do not substitute a bitwise round-trip promise for a lossy conversion or rounded
composition. Report actual execution separately from these future obligations.
Sources are pinned with each formula. Physical validity outside nominal domains,
browser bit identity, device CMYK, future BT.2020 CL and complete rendering are
not claimed. No operator-local interview question remains; schema/runtime work
and the separate future CL member remain explicit dependencies.
