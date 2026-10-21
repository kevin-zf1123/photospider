---
spec_schema_version: 1
id: FMT-09
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
---

# FMT-09: transfer encoding and decoding

Inherit [FMT-common](FMT_common_contract.md), the NUM numerical/execution baseline,
[model coverage](FMT_model_conversion_coverage.md) and
[canonical image/codec boundary](FMT_codec_boundary.md). The family contains [A decode](FMT-09A_decode_transfer.md) and
[B encode](FMT-09B_encode_transfer.md). Each direction has strict, accelerated
Apple Silicon and accelerated x86-64 CPU fixed invocation ABI 8 keys. Their exact scalar
definitions are normative in [FMT-09 mathematics](FMT-09_transfer_math.md). The
specification remains Proposed; registration and implementation status are separate. The fixed invocation, tile coverage and caller-precondition rules below govern this family in place of inherited window, validation and dirty-mapping execution descriptions.

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

NUM/CRV tuple descriptions use the shared TensorDescription and transfer codecs.
Their member admission remains restricted to linear, sRGB and power gamma
(YCbCr requires an encoded transfer). Numeric ramps consume and emit straight
coordinates; FMT-09 independently provides the wider transfer set below. Sharing
a codec does not make every transfer legal for every consumer.

## Confirmed curve set, meaning and domains

D denotes A decoding and E denotes B encoding. Semantic callers guarantee finite participating inputs, membership in the listed input domains and finite rounded outputs throughout complete retained output tiles. These are unchecked sample-domain preconditions. Static description structure, parameter presence/types and codec syntax are checked. Numeric gamma/Lb/Lw domains are unchecked premises even during preparation.

| Curve | Native linear quantity | Semantic input domains and selected behavior |
| --- | --- | --- |
| linear | Preserve coherent native units/reference, relative or absolute. | Any finite value; exact bit-copy identity, subject to the finite caller precondition. |
| power_gamma | Explicit relative scene or display quantity. | Any finite input in either direction; finite gamma>0 required. Apply magnitude power then restore sign; values above 1 are not clipped. |
| srgb | Explicit relative scene or display quantity. | Any finite input; signed extension with published piecewise constants, no gamut/black normalization or monitor-luminance scaling. |
| bt709 | Scene-relative, inverse/source OETF pair. | Any finite input; published BT.709-6 constants plus signed extension. Does not select BT.1886 implicitly. |
| bt2020 | Scene-relative, inverse/source OETF pair. | Any finite input; signed extension; static smooth/rounded_10bit/rounded_12bit variant, smooth by default. |
| bt1886 | Absolute display-linear component scale in cd/m². | D accepts [0,1]; E accepts [Lb,Lw]. Explicit finite 0<=Lb<Lw; standard exponent fixed at 2.4. |
| pq | Absolute display-linear component scale in cd/m². | D accepts [0,1]; E accepts [0,10000]. EOTF/inverse only, fixed 10000 scale, no reference OOTF. |
| hlg_oetf | Normalized scene-linear quantity. | Both directions accept their respective [0,1] inputs. OETF/inverse only; no system gamma, black lift or full display EOTF/OOTF. |
| acescc | Scene-relative scalar transfer, basis stated separately. | Any finite input. Preserve the official nonpositive encoding floor and decoding cap at 65504. |
| acescct | Scene-relative scalar transfer, basis stated separately. | Any finite input. Preserve the negative linear toe and official decoding cap at 65504. |

No output-domain clamp is added. This valid semantic decode
can exceed the next semantic encoder's [0,1] input domain in Float64, so that
next call is outside its semantic input precondition. Preserve the formula, not an unrequested normalization or
endpoint fix. A caller needing [0,1] applies an explicit clamp. No full-range
validity proof or exact round-trip promise is attached to such an output.

The signed extension belongs only to gamma/sRGB/709/2020. PQ/HLG semantic domains remain caller obligations. ACES intrinsic floors/caps remain in both Float32 and
Float64; no unbounded variant is introduced under the same identifier. Vendor
camera logs require separately versioned future definitions. Ordinary log2/log10
math remains in NUM.

## Tensor interface and static parameters

One input Result with one tensor member  produces one `values`
output Result containing one tensor. The member must use Float32 or Float64; the
output retains the dtype, logical shape, axes and channel order. Complete sample
rank, including batch and cell axes, is at most 8; sample count is at most 2^40.
The operation preserves Result schema identity, tensor key, logical shape and
batch axes. It carries no description and sets `atomic_trailing_axes` to zero. No additional resource port, optional data input or runtime-dependent
shape is introduced.
Rank and extent validity inherit NUM/kernel. No implicit broadcasting, casting,
axis movement, range scaling or integer decoding occurs. In semantic mode,
non-native numeric storage encoding, including Float32 carrying 0..255 codes,
must be explicitly converted to the curve's native scalar coordinates through
FMT-06 first. Raw floating calls apply the formula to stored numeric values
without requiring that semantic decoding; integer dtype remains unsupported in
all modes.

With `input_description`, the described path processes one explicitly selected RGB or Gray
group. A Gray group without a channel axis is its one component; an explicit
channel axis can occupy any structurally valid position. `axis` indexes a cell
axis and excludes the Result batch prefix. The group selector and axis
interpretation resolve statically and uniquely. Gray must describe the
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
| input_description | Optional explicit TensorDescription. Presence selects the described RGB/Gray path; absence selects raw numerical components. |
| group | Required static unique RGB/Gray group when selecting an explicit group. An axis-free Gray component may supply its component description without a group. |
| components / axis | Raw all-components or explicit index selection; do not also supply semantic group. |
| curve | A may resolve from input_description; an explicit assertion must agree. B and raw require an explicit curve. No default sRGB guess. |
| gamma | Required only for power_gamma; finite static Float64 >0; no implicit 2.2 exponent. |
| coefficient_variant | Only for bt2020: smooth (authoring default), rounded_10bit or rounded_12bit. |
| black_luminance / white_luminance | Required only for bt1886; finite static Float64 Lb,Lw in cd/m² with 0<=Lb<Lw. |
| layout | auto by default; view or materialize explicitly selected. |

Irrelevant, contradictory or malformed parameters fail rather than being ignored.
A omitted curve takes the complete transfer record, including its parameters,
from input_description. When a curve is explicitly supplied, its resolved defaults and
parameters form a full assertion. Thus an explicit bt2020 with omitted variant
means smooth, and conflicts with a rounded_10bit source unless the caller supplies a different explicit description.
B's target defaults never repair or overwrite an inconsistent source implicitly.
Curve, variant and layout are closed String choices in authoring;
group and raw selectors are typed static metadata/axis selectors, with bounded
integer indices. The public `TransferDefinition` codec encodes complete transfer identities in
bounded `fmt09-v1:` records for explicit tensor-description configuration. Parameter-free
curves use their canonical identifier; parameterized records preserve exact
Float64 parameter bits. Raw selectors use a bounded canonical index string.

## Curve identity, metadata and reference checks

The described implementation resolves compatible global/group/selected-channel
assertions, explicit analytic bindings and native numeric encodings. It supports
axis-free Gray groups as well as a component-only Gray description. Gray must
supply `coordinates.gray_kind`: `linear_y` for encode or linear decode, and
`encoded_luma` for a nonlinear decode. CIELAB/OKLab lightness is not transfer
luminance. Canonical `reference=scene|display` requires explicit
`coordinates.scale=relative|absolute`; legacy relative/display-absolute reference
spellings remain accepted, with contradictory scales rejected. Relative units
accept `relative`/`1` (Gray also `relative_luminance`), and absolute display units
accept `cd/m2`/`cd/m^2`. Spatial selection must agree with the structural channel
axis. The native view helper uses these same semantic rules.

The described path reads only the explicit `input_description`. It requires a
unique `group` (or an explicit axis-free Gray component) and rejects `axis/components`. Decode A resolves the complete
source transfer when `curve` is omitted; any supplied curve and its parameters
must agree with the description. Encode B always requires a target curve and
requires the explicit source transfer to be linear. No sRGB guess or automatic
scene/display conversion occurs.

Gamma/sRGB preserve the chosen scene-relative or display-relative scale.
709/2020 OETF, HLG OETF and ACES scalar curves require scene-relative quantities;
PQ/1886 require absolute display-linear quantities. Curves are independent of
RGB primaries: applying an ACEScct scalar curve does not establish a complete
ACEScct color space. All native units/reference checks remain in preparation.

Without `input_description`, raw calls require `components` and an explicit
curve. An index list requires `axis`; `components=all` may omit it. Raw formula
evaluation uses its defined IEEE outcomes, while intrinsic PQ/1886
max and ACES cap/floor branches still execute. Static parameter and storage
checks apply in both paths.

Neither path retains or creates a description in the output. A computes the
curve's linear quantity and B computes the target encoded quantity; callers
provide the corresponding configuration to later consumers. Description
parameters affect computation identity but do not certify unrequested samples.

## Fixed tile dependencies and identity

Each retained output tile produces one fixed CPU task. Every coordinate is initialized: a participating coordinate evaluates its transfer, and an unselected coordinate copies bits. The complete output region is closed over the input's own TileGrid, including any difference in atomic trailing axes. Preparation verifies exactly those read slots. Extra samples granted by source-grid closure are not evaluated and do not extend semantic caller preconditions. Intrinsic floors/caps retain their source dependencies because branch selection uses the actual input.

Static description, parameter, shape and profile checks apply even to Empty. An Empty output request emits no payload task. A tile containing only bypass components performs no transfer arithmetic. Hidden straight color remains participating when alpha is zero; the curve has no alpha input dependency.

Only linear and gamma=1 are whole-mapping static identities. They copy bits before NaN classification, preserving signaling NaNs and signed zeros. Semantic callers still guarantee finite participating identity samples in retained output tiles. Neither a bypass-only request nor a runtime cap/floor selects the identity exception.

`auto` and `materialize` both write independent dense row-major output tiles. Forced `view` rejects a statically nonidentity curve during preparation. A nonempty identity `view` task depends only on the input schema and returns `InvalidArgument/ViewUnavailable`. It acquires no source payload and publishes no Result. Empty identity view has no task.

## Numerical scratch, ownership and errors

The Plan declares output and scratch allocations before execution. Copy-only tasks use zero scratch. Gamma=2 uses a fixed 64-sample gather/result/offset block and preserves the exact signed square/sqrt algorithm, including integer NaN handling and the existing SIMD capability gate. General curves add fixed fast, compact and wide numerical workspaces plus branch-gather arrays. Execution processes at most 64 points per block; fast candidates gather at most 16 operands from the chosen branch.

Preparation freezes the real-expression DAG and exact branch thresholds. Rational branches are compiled into degree-at-most-two polynomials with signed integer numerators and one positive common denominator, each bounded to 64 bits, optionally followed by sqrt. Runtime forms the polynomial in units of 2^-2148 using fixed 4352-bit integers. A finite binary64 input uses fewer than 2098 bits in units of 2^-1074; the polynomial magnitude uses fewer than 4262 bits, and the final division/root midpoint products fit the workspace. Static constant reduction precedes the coefficient-size bound.

Transcendental branches retain directed interval refinement, compact 128/256-bit attempts and wide attempts up to 4096-bit working precision. Matching rounded endpoints are required for strict results. An explicit internal-capacity or unresolved-precision outcome may select the wide evaluator. Cancellation and caller resource failures, even if they use the same status code as internal capacity exhaustion, propagate without retry. All runtime numerical storage belongs to invocation scratch; no dynamic rational construction, heap allocation or exception-driven refinement is used.

The host retains immutable preparation, source views, exclusive output and scratch through completion. Numerical caches are local to the invocation. Cancellation is polled at least every 64 visited points and within bounded exact/refinement loops. Every return restores the floating environment. The executor publishes successful retained Result values at ordered Call retirement; failed calls publish no successful Result. Currentness, admission, missing coverage and backend failures follow the physical Plan contract. The family provides no runtime numerical diagnostic stream or Result cache.

| Condition | Phase | Outcome |
| --- | --- | --- |
| Malformed/missing/irrelevant parameter or wrong type, ambiguous selector, source assertion conflict | Static preparation | InvalidArgument / InvalidDomain. |
| Unsupported dtype/model, incompatible units/reference, wrong source transfer or structural layout | Static preparation | TypeMismatch. |
| Semantic nonfinite/out-of-domain input or nonfinite rounded output | Caller sample precondition | Outside the fixed invocation contract; no sample error guaranteed. |
| Forced view of a statically nonidentity curve | Static preparation | InvalidArgument / InvalidDomain, ViewUnavailable message. |
| Nonempty identity forced view | Invocation | InvalidArgument / ViewUnavailable. |
| Raw domain/nonfinite/overflow arithmetic | Evaluation | Defined IEEE result of the selected formula. |
| Capacity/refinement exhaustion, missing coverage, cancellation/currentness or backend failure | Corresponding preparation/execution phase | Preserve operational status. |

## Support matrix

| Dimension | Support | Boundary |
| --- | --- | --- |
| Profiles | Strict, accelerated Apple Silicon and accelerated x86-64 CPU keys in each direction; NUM precision and ISA rules apply. | No automatic dispatcher, unsuffixed alias or GPU implementation. |
| Samples | Float32/Float64, same dtype throughout; semantic RGB/compatible Gray or explicit raw components. | No implicit integer decoding, casts or range normalization. |
| Storage | Generic/spatial input with admitted signed/zero strides; independent dense output tiles; unchanged shape and axes. | Logical spatial axes do not require a full-image contiguous allocation. |
| Dependencies | Fixed input-grid closure of complete retained output tiles, or schema-only for forced identity view. | No sample-dependent dependency discovery or alpha shortcut. |
| Publication | All samples of each retained output tile, at unchanged global coordinates. | Unproduced samples remain missing. |
| Determinism | Immutable curve/reference/parameters and explicit numerical profile. | No external display/device interpretation or color engine. |

## Acceptance and implementation dependencies

Use independent high-precision real-function evaluation and exact rational
constants for strict rounding. Do not compare against a second call to these
operators as the oracle. Reference cases cover every branch threshold and its
adjacent Float32/64 values, signed zeros, gamma=1, negative/HDR extensions,
nonfinite raw results, valid semantic-domain fixtures, intrinsic plateaus/caps, BT.2020
variants and explicit reference/parameter conflicts. Members add concrete cases.

Compare full, offset/disjoint, participating-only and bypass-only output tile selections, differing input/output grids, Empty, source immutability, retained owner lifetime, concurrent Runs, artifact import, malformed task rejection, zero runtime allocation, scratch canaries, cancellation and floating-environment restoration. Test native identity bit copies and schema-only view failure separately. Semantic fixtures satisfy every retained participating sample's preconditions; raw fixtures verify NaN payloads, infinities, signed zeros and intrinsic plateaus without a semantic-domain filter.

Keep the independent high-precision oracle and remove temporary built-in execution checkers after verification. Future camera-log variants require separately specified transfer definitions.
