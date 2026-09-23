---
spec_schema_version: 1
id: FMT-09
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-09: transfer encoding and decoding

Inherit [FMT-common](FMT_common_contract.md), the NUM numerical/execution baseline,
[model coverage](FMT_model_conversion_coverage.md) and
[canonical image/codec boundary](FMT_codec_boundary.md). The family contains
[A decode](FMT-09A_decode_transfer.md) and [B encode](FMT-09B_encode_transfer.md),
both proposed native primitives. Their exact scalar definitions are normative in
[FMT-09 mathematics](FMT-09_transfer_math.md). Clarification is complete for the
selected scope; no runtime registration or implementation is delivered here.

## Purpose and responsibilities

A decodes a specified transfer into that curve's defined linear quantity. B
performs the corresponding encoding. Curve identity and parameters are static.
Changing them requires normal reinference/compilation. An encoded-to-encoded
conversion explicitly composes A and B with any required intervening stages.

RGB basis/white changes belong to FMT-10, model transforms to FMT-11,
dtype/interval/code operations to FMT-06, and complete tone/view rendering to
FMT-13/15 as explicitly selected. I/O codecs own external packing and chroma
sampling. FMT-09 does not implicitly execute those stages or normalize an
arbitrary luminance to 1. A transfer curve does not itself change primaries,
white or turn scene coordinates into display rendering.

Current ColorTransferKind in
[color_array.hpp](../../../../include/photospider/data/color_array.hpp) only
contains linear, sRGB and gamma. NUM/CRV ramp helpers do not implement independent
FMT-09 nodes or the new units/reference vocabulary. Their old association and
last-axis conventions do not override the FMT contract.

## Confirmed curve set, meaning and domains

D denotes A decoding and E denotes B encoding. All semantic participating inputs
must be finite; listed domains are additional input checks. The selected output
must round to a finite result. Static validity does not replace sample checks.

| Curve | Native linear quantity | Semantic input domains and selected behavior |
| --- | --- | --- |
| linear | Preserve coherent native units/reference, relative or absolute. | Any finite value; exact bit-copy identity, subject to semantic validation. |
| power_gamma | Explicit relative scene or display quantity. | Any finite input in either direction; finite gamma>0 required. Apply magnitude power then restore sign; values above 1 are not clipped. |
| srgb | Explicit relative scene or display quantity. | Any finite input; signed extension with published piecewise constants, no gamut/black normalization or monitor-luminance scaling. |
| bt709 | Scene-relative, inverse/source OETF pair. | Any finite input; published BT.709-6 constants plus signed extension. Does not select BT.1886 implicitly. |
| bt2020 | Scene-relative, inverse/source OETF pair. | Any finite input; signed extension; static smooth/rounded_10bit/rounded_12bit variant, smooth by default. |
| bt1886 | Absolute display-linear component scale in cd/m². | D accepts [0,1]; E accepts [Lb,Lw]. Explicit finite 0<=Lb<Lw; standard exponent fixed at 2.4. |
| pq | Absolute display-linear component scale in cd/m². | D accepts [0,1]; E accepts [0,10000]. EOTF/inverse only, fixed 10000 scale, no reference OOTF. |
| hlg_oetf | Normalized scene-linear quantity. | Both directions accept their respective [0,1] inputs. OETF/inverse only; no system gamma, black lift or full display EOTF/OOTF. |
| acescc | Scene-relative scalar transfer, basis stated separately. | Any finite input. Preserve the official nonpositive encoding floor and decoding cap at 65504. |
| acescct | Scene-relative scalar transfer, basis stated separately. | Any finite input. Preserve the negative linear toe and official decoding cap at 65504. |

No output-domain clamp is added. HLG's published rounded a=0.17883277 gives
D(1) approximately 1.000000026934807 before rounding. This valid semantic decode
can exceed the next semantic encoder's [0,1] input domain in Float64, so that
next call rejects it. Preserve the formula, not an unrequested normalization or
endpoint fix. A caller needing [0,1] applies an explicit clamp. No full-range
validity proof or exact round-trip promise is attached to such an output.

The signed extension belongs only to gamma/sRGB/709/2020. PQ/HLG semantic
out-of-domain values fail. ACES intrinsic floors/caps remain in both Float32 and
Float64; no unbounded variant is introduced under the same identifier. Vendor
camera logs require separately versioned future definitions. Ordinary log2/log10
math remains in NUM.

## Tensor interface and static parameters

One required tensor Value input `input` produces one tensor Value output
`values` under the target generic-metadata/planar contract. No Result schema,
resource port, optional data input or runtime-dependent shape is introduced.
Both retain the same Float32 or Float64 dtype, rank/extents, axis positions and channel order.
Rank and extent validity inherit NUM/kernel. No implicit broadcasting, casting,
axis movement, range scaling or integer decoding occurs. In semantic mode,
non-native numeric storage encoding, including Float32 carrying 0..255 codes,
must be explicitly converted to the curve's native scalar coordinates through
FMT-06 first. Raw floating calls apply the formula to stored numeric values
without requiring that semantic decoding; integer dtype remains unsupported in
all modes.

Semantic respect/override mode processes one explicitly selected RGB or Gray
group. A Gray group without a channel axis is its one component; an explicit
channel axis can occupy any structurally valid position. The group selector and
axis interpretation resolve statically and uniquely. Gray must describe the
curve-compatible native quantity; normalized CIELAB l, YCbCr or CMYK components do not
become linear color by selection. Other groups, alpha and AOVs pass through
bit-for-bit. Complete images stay straight with internal alpha.

Raw mode selects ordinary numeric components explicitly: either all components
of a tensor, or a nonempty unique static index list on an explicit/resolved axis.
A whole single-component plane needs no invented singleton channel axis. These
are structural selections, not an implicit color-group inference. Output shape
and dtype remain unchanged, and unselected samples copy exactly.

| Logical parameter | Rule |
| --- | --- |
| metadata_mode | respect by default; override or raw explicitly selected. |
| group | Required static unique RGB/Gray group in semantic mode. |
| components / axis | Raw all-components or explicit index selection; do not also supply semantic group. |
| curve | A may resolve from input metadata; an explicit assertion must agree in respect. B and raw require an explicit curve. No default sRGB guess. |
| gamma | Required only for power_gamma; finite static Float64 >0; no implicit 2.2 exponent. |
| coefficient_variant | Only for bt2020: smooth (authoring default), rounded_10bit or rounded_12bit. |
| black_luminance / white_luminance | Required only for bt1886; finite static Float64 Lb,Lw in cd/m² with 0<=Lb<Lw. |
| metadata_override | Complete effective source interpretation when override is selected; cannot change actual dtype, shape or backing. |
| layout | auto by default; view or materialize explicitly selected. |

Irrelevant, contradictory or malformed parameters fail rather than being ignored.
A omitted curve takes the complete transfer record, including its parameters,
from the source. When a curve is explicitly supplied, its resolved defaults and
parameters form a full assertion. Thus an explicit bt2020 with omitted variant
means smooth, and conflicts with a rounded_10bit source unless overridden.
B's target defaults never repair or overwrite an inconsistent source implicitly.
Mode, curve, variant and layout are closed String choices in authoring;
group and raw selectors are typed static metadata/axis selectors, with bounded
integer indices. The existing metadata codec cannot encode these new records
unchanged. These are logical static fields; final canonical codec serialization remains an
implementation dependency, not an invented current API.

## Curve identity, metadata and reference checks

A's effective source transfer must match the chosen curve and parameters. A
publishes its native linear descriptor. B requires source transfer=linear and
compatible native units/reference, then publishes the explicit target transfer.
Applying B to already encoded data fails even if the target curve is identical.
A cannot use a non-linear curve to decode an explicitly linear source in respect
mode. The linear curve itself is admitted as the explicit identity.

Gamma/sRGB preserve explicit scene-relative or display-relative interpretation.
709/2020 OETF, HLG OETF and ACES scalar curves require scene-relative native
quantities; PQ/1886 require absolute display-linear quantities. Encoded metadata
retains that native interpretation even where stored samples are dimensionless.
No transfer call silently converts scene to display reference. Source assertions
must agree; deliberate reinterpretation uses override or raw.

Curves are independent of RGB primaries. P3 may use sRGB transfer; another RGB
basis may use the ACEScct scalar curve without becoming the full ACEScct space.
Preserve primaries, white, channel roles/names, coordinates, internal alpha and
unaffected groups. Update the selected group's transfer, native unit/reference
and actual numeric-encoding description. Incompatible complete named-space
labels cannot survive; update or remove them. A full named-space claim validates
basis, white, transfer and reference together, using explicit FMT-10 conversion
where needed. Static structural checks do not validate all pixel values.

Raw requires the chosen curve/parameters explicitly. It omits sample finiteness
and semantic-domain checks, using NUM formula outcomes. It retains still-applicable
source descriptions without previous validity guarantees and does not change
transfer metadata to claim a valid color conversion. Intrinsic curve branches,
including PQ/1886 max and ACES cap/floor, still execute. Static parameter checks
and physical storage constraints apply in every mode.

## Exact requests, validation and invalidation

Let Q be requested output coordinates and T their intersection with participating
components. Data is input[Q]. Semantic Validation covers input[T] and finite
rounded transformed results; no unrequested component or alpha sample is read.
There is no per-pixel Control dependence. All required static descriptions and
parameters are checked regardless of requested pixels, including Empty requests.
An Empty request does not acquire sample windows or run arithmetic.

Every transformed component uses only itself. Do not skip hidden straight color
because alpha=0; alpha is not read to make such a decision. Alpha/AOV-only and
unselected-group requests are exact copies without transfer-domain validation.
Invalid unrequested peers do not fail another component's observation. A required
upstream Whole operation retains its own dependency and failure extent.

An input dirty set maps identically to output coordinates; there is no alpha or
cross-channel fanout. Keep these input witnesses even on intrinsic constant
branches such as ACES caps. Static description/parameter changes require
reinference and invalidate affected compiled identity. Failures stay within the
normal observation model; no global validity scan or partial success for a
failed observation is introduced.

## Identity, storage, resources and errors

Linear and gamma=1 are static bit-copy identities, including raw NaN payloads and
signed zero. Semantic identity still checks requested participating samples and
publishes the appropriate output metadata. Auto may share a legal read-only
owner only when the whole transformation is a static bit identity; forced view
otherwise fails. A request limited to alpha does not turn a nonidentity curve
into a view-capable operation. Constant/cap branches are not static identities.

All other curves materialize requested coverage in a new result owner. Materialize
forces this even for identities. Canonical images reserve their complete virtual
span under the one DAG tile geometry; only required pages receive backing.
Valid edge rows, tile-width padding, page alignment, explicit preparation and
retention follow kernel storage. Generic non-image raw tensors use their admitted
tensor storage. A shared view publishes only the admitted result coverage and
its actual validation scope, not blanket validity of the backing.

Resolve static selectors/metadata first, map exact input demand, acquire retained
read windows, admit output/scratch/pages, evaluate or copy requested coordinates,
then check cancellation/currentness before publication. Owners and produced
coverage survive the normal producer/context lifetime. No eviction, replay,
external alpha provenance or unpublished source recovery is added.

For N requested samples, rank r and F windows, addressing costs O(N*r+F*r), plus
static metadata and curve arithmetic. Strict transcendental/coefficient work and
scratch must be charged and bounded under NUM; exhaustion fails without silently
switching to approximate math. No per-pixel metadata or eager full-image payload
scan is allowed. SIMD must not read padding, peer channels or unprepared pages.
Poll cancellation/currentness at most every 1024 visited entries and inside
bounded multiprecision refinement, as well as before publication. Release
unpublished buffers/pages/scratch on failure. Deduplicate retained owners when
accounting for actual backing, windows, outputs and numeric workspace; virtual
address reservation, logical bytes and resident backing are distinct metrics.

Initial optional result caching remains disabled until it can preserve exact
coverage, metadata and validation identity. A future cache key must include
source identity, direction, curve/version/variant, all parameters, group/raw
selection, effective metadata/mode, dtype/layout and CPU numerical profile.
Do not share raw/semantic validity or merge distinct 709/2020 descriptions merely
because one selected coefficient set is numerically equal. NUM backend selection,
fallback diagnostics, exact copy rules and precision bounds remain unchanged.

| Condition | Phase | Outcome |
| --- | --- | --- |
| Malformed/missing/irrelevant parameter, invalid gamma/Lb/Lw, ambiguous selector, source assertion conflict | Compile/direct preflight | InvalidArgument / InvalidDomain. |
| Unsupported dtype/model, incompatible units/reference, wrong source transfer state or structural layout | Static preflight | TypeMismatch / None. |
| Requested semantic input nonfinite or outside curve domain | Requested evaluation | OperationFailed / InvalidDomain. |
| Finite semantic formula rounds to a nonfinite result | Requested evaluation | OperationFailed / ArithmeticOverflow. |
| Forced view of nonidentity or unrepresentable backing | Normal layout check | InvalidArgument / InvalidDomain; ViewUnavailable. |
| Raw domain/nonfinite/overflow arithmetic | Raw evaluation | NUM-defined result, except static parameter failures. |
| Resource/work, missing coverage, cancellation/currentness or backend failure | Inherited phase | Preserve existing status; no semantic fallback hides it. |

## Target support matrix

| Dimension | Target support | Not implied |
| --- | --- | --- |
| Numeric profiles | Three default-registry CPU entries per direction: strict, accelerated Apple Silicon and accelerated x86-64. NUM defines their precision, ISA checks and explicit fallback reporting. | No automatic profile dispatcher, unsuffixed alias or GPU implementation. |
| Samples | Float32/Float64; same dtype throughout one tensor. Semantic RGB/compatible Gray; explicit raw components. | Integer decoding, Float16, mixed per-channel dtype or implicit complex math. |
| Storage | Canonical planar images and valid generic non-image raw tensor layouts. Shape/axes unchanged. | Interleaved images, per-node tile geometry or metadata-only storage adaptation. |
| Demand | Exact per-output-coordinate Data and dirty mapping, selected-component Validation, static Descriptor dependence; no halo or pixel Control read. | Whole fallback claimed as exact FMT support. |
| Publication | Exact Q coverage at unchanged global coordinates with ordinary retained immutable owner/read windows. | Unrequested samples as implicit zero, whole-owner sample-validity certificates or hidden recovery. |
| Determinism | Static curve/reference/parameters and explicit NUM numerical profile. | Display-device queries, implicit environment/view settings or external color engines. |

Requested errors retain node/output and logical sample attribution under the
inherited failure model. Missing coverage remains missing; padding and unproduced
samples never become ExplicitZero. Disjoint observations preserve their own
success/failure scope and required upstream dependencies.

## Acceptance and implementation dependencies

Use independent high-precision real-function evaluation and exact rational
constants for strict rounding. Do not compare against a second call to these
operators as the oracle. Reference cases cover every branch threshold and its
adjacent Float32/64 values, signed zeros, gamma=1, negative/HDR extensions,
nonfinite raw results, semantic rejection, intrinsic plateaus/caps, BT.2020
variants and explicit reference/parameter conflicts. Members add concrete cases.

Compare full, offset/disjoint, R-only, alpha/AOV-only and cross-tile requests,
including y/x=[127,130) under tile size 128. Put NaN or invalid-domain samples in
unrequested peers, then request them separately. Test hidden color at zero alpha,
Whole upstream failures, exact dirty support, Empty, source immutability,
view/materialize equality, retained owner lifetime, budgets/cancellation and
floating-environment restoration. A view must not bypass identity validation.

When implemented, provide public compile/execute examples and correctness-gated
benchmarks for Float32/64 [4096,4096,4] planar inputs, full/R-only/alpha-only
requests and sparse tile-crossing ROIs. Record concrete profile/ISA/build,
workers, page state, time, logical bytes, reserved span, backing and scratch.
No runtime throughput or conformance result is claimed by this specification.
Implementation needs canonical group/transfer/unit metadata, exact partial
component dispatch/publication, curve kernels and bounded strict math support.
The existing ColorArray codec and bounded planar CPU subset are not conformance
evidence. Future camera-log variants need their own clarification.
