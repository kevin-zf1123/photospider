---
spec_schema_version: 1
id: FMT-09
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_result_abi_4
clarification_status: complete
---

# FMT-09: transfer encoding and decoding

Inherit [FMT-common](FMT_common_contract.md), the NUM numerical/execution baseline,
[model coverage](FMT_model_conversion_coverage.md) and
[canonical image/codec boundary](FMT_codec_boundary.md). The family contains [A decode](FMT-09A_decode_transfer.md) and
[B encode](FMT-09B_encode_transfer.md). Each direction has strict, accelerated
Apple Silicon and accelerated x86-64 CPU Result ABI 4 keys. Their exact scalar
definitions are normative in [FMT-09 mathematics](FMT-09_transfer_math.md). The
specification remains Proposed; registration and implementation status are separate.

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
[color_array.hpp](../../../../include/photospider/ops/color_array.hpp) only
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

No output-domain clamp is added. This valid semantic decode
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

One input Result with one tensor member and no fields produces one `values`
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
| group | Required static unique RGB/Gray group in semantic mode. |
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

The described path reads only the explicit `input_description`. It requires a
unique `group` and rejects `axis/components`. Decode A resolves the complete
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
evaluation omits described-domain finiteness checks, while intrinsic PQ/1886
max and ACES cap/floor branches still execute. Static parameter and storage
checks apply in both paths.

Neither path retains or creates a description in the output. A computes the
curve's linear quantity and B computes the target encoded quantity; callers
provide the corresponding configuration to later consumers. Description
parameters affect computation identity but do not certify unrequested samples.

## Exact requests, validation and invalidation

Let Q be requested output coordinates and T their intersection with participating
components. Descriptor support (role 8) covers the connected input Result.
Pass-through components and raw selections request Data (role 1); semantic
participating components request Data plus Validation (roles 1 and 4). Semantic
Validation covers input[T] and finite rounded transformed results; no unrequested
component or alpha sample is read. There is no Control dependence. All required static descriptions and
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
signed zero. Semantic identity still validates the selected samples and publishes
the appropriate transfer metadata. Auto may share a legal read-only owner when
the complete declared transformation is a static identity. This applies to legal
generic and spatial Result storage. A request limited to alpha does not make a
nonidentity transformation viewable; constant/cap branches are not static
identities. Forced view rejects a statically nonidentity mapping during compile
or direct preflight, and rejects an identity whose requested physical mapping
cannot be proven during observation evaluation.

All nonidentity curves materialize requested coverage in a new Result owner.
`materialize` forces this even for identities. Result storage and publication
follow the declared generic or spatial layout; only requested coverage is
produced. A shared view publishes only admitted Result coverage and its actual
validation scope, not blanket validity of the backing.

Preparation resolves selectors, metadata and the immutable curve program without
retaining per-run payload state. During execution, map exact input demand, acquire
read windows, admit output pages and lazily allocate numerical workspace, then
check cancellation/currentness before publication. Owners and produced coverage
survive the normal producer/context lifetime. Empty output demand is stateless.

For N requested samples, rank r and F windows, addressing costs O(N*r+F*r), plus
static metadata and curve arithmetic. Strict transcendental/coefficient work and
scratch must be charged and bounded under NUM; exhaustion fails without silently
switching to approximate math. No per-pixel metadata or eager full-image payload
scan is allowed. SIMD must not read padding, peer channels or unprepared pages.
Poll cancellation/currentness within bounded runs of at most 1024 visited entries,
inside bounded multiprecision refinement and before publication. Simple copy,
identity and gamma=2 spans can process up to 1024 samples per run; the general
curve path batches at most 64 samples. Numerical workspace is allocated lazily
through the observation allocator, and identity/copy paths need no math workspace.
The floating-point environment is established for execution and restored on exit.
Accelerated profiles retain strict fallback and NUM diagnostics. Release
unpublished buffers/pages/scratch on failure. Deduplicate retained owners when
accounting for actual backing, windows, outputs and numeric workspace; virtual
address reservation, logical bytes and resident backing are distinct metrics.

Result caching is disabled. Raw and semantic validity are separate, and distinct
709/2020 transfer descriptions remain distinct even when a coefficient set is
numerically equal. NUM backend selection, fallback diagnostics, exact copy rules
and precision bounds remain unchanged.

| Condition | Phase | Outcome |
| --- | --- | --- |
| Malformed/missing/irrelevant parameter, invalid gamma/Lb/Lw, ambiguous selector, source assertion conflict | Compile/direct preflight | InvalidArgument / InvalidDomain. |
| Unsupported dtype/model, incompatible units/reference, wrong source transfer state or structural layout | Static preflight | TypeMismatch / None. |
| Requested semantic input nonfinite or outside curve domain | Requested evaluation | OperationFailed / InvalidDomain. |
| Finite semantic formula rounds to a nonfinite result | Requested evaluation | OperationFailed / ArithmeticOverflow. |
| Forced view of a statically nonidentity mapping | Compile/direct preflight | InvalidArgument / InvalidDomain; ViewUnavailable. |
| Forced view of an identity mapping whose physical representation cannot express the requested view | Observation evaluation | InvalidArgument / InvalidDomain; ViewUnavailable. |
| Raw domain/nonfinite/overflow arithmetic | Raw evaluation | NUM-defined result, except static parameter failures. |
| Resource/work, missing coverage, cancellation/currentness or backend failure | Inherited phase | Preserve existing status; no semantic fallback hides it. |

## Target support matrix

| Dimension | Target support | Not implied |
| --- | --- | --- |
| Numeric profiles | Three default-registry CPU entries per direction: strict, accelerated Apple Silicon and accelerated x86-64. NUM defines their precision, ISA checks and explicit fallback reporting. | No automatic profile dispatcher, unsuffixed alias or GPU implementation. |
| Samples | Float32/Float64; same dtype throughout one tensor. Semantic RGB/compatible Gray; explicit raw components. | Integer decoding, Float16, mixed per-channel dtype or implicit complex math. |
| Storage | Legal generic and spatial Result tensor layouts; shape and axes unchanged. | Interleaved image semantics, per-node tile geometry or metadata-only storage adaptation. |
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

Validate the Result operations with independent high-precision references,
partial-component and ROI requests, Empty demand, identity views, owner lifetime,
resource limits, cancellation and floating-environment restoration. Future camera-log
variants require separately specified transfer definitions; they are outside
these registered keys.
