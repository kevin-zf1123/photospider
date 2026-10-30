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

Inherits [FMT-common](FMT_common_contract.md), the NUM numerical baseline, [model coverage](FMT_model_conversion_coverage.md) and the [canonical image/codec boundary](FMT_codec_boundary.md). The family contains [A decode](FMT-09A_decode_transfer.md) and [B encode](FMT-09B_encode_transfer.md). Each direction has strict, accelerated Apple Silicon and accelerated x86-64 CPU keys. Exact scalar definitions are normative in [FMT-09 mathematics](FMT-09_transfer_math.md).

## Purpose

A decodes a specified transfer into that curve's defined linear quantity. B performs the corresponding encoding. Curve identity and parameters are static. An encoded-to-encoded conversion explicitly composes A and B with any required intervening stages.

RGB basis/white changes belong to FMT-10, model transforms to FMT-11, dtype/interval/code operations to FMT-06, and complete tone/view rendering to FMT-13/15. I/O codecs own external packing and chroma sampling. FMT-09 executes none of those stages and does not normalize an arbitrary luminance to 1. A transfer curve does not change primaries, white or turn scene coordinates into display rendering.

NUM/CRV tuple descriptions use the shared TensorDescription and transfer codecs. Their member admission is restricted to linear, sRGB and power gamma (YCbCr requires an encoded transfer). FMT-09 independently provides the wider transfer set below; sharing a codec does not make every transfer legal for every consumer.

## Curves and domains

D denotes A decoding and E denotes B encoding. Semantic callers guarantee finite participating inputs, membership in the listed input domains and finite rounded outputs. These are unchecked sample-domain preconditions. Static description structure, parameter presence/types and codec syntax are checked. Numeric gamma/Lb/Lw domains are unchecked premises.

| Curve | Native linear quantity | Semantic input domains and selected behavior |
| --- | --- | --- |
| linear | Preserve coherent native units/reference, relative or absolute. | Any finite value; exact bit-copy identity, subject to the finite caller precondition. |
| power_gamma | Explicit relative scene or display quantity. | Any finite input in either direction; finite gamma>0 required. Apply magnitude power then restore sign; values above 1 are not clipped. |
| srgb | Explicit relative scene or display quantity. | Any finite input; signed extension with published piecewise constants, no gamut/black normalization or monitor-luminance scaling. |
| bt709 | Scene-relative, inverse/source OETF pair. | Any finite input; published BT.709-6 constants plus signed extension. Does not select BT.1886. |
| bt2020 | Scene-relative, inverse/source OETF pair. | Any finite input; signed extension; static smooth/rounded_10bit/rounded_12bit variant, smooth by default. |
| bt1886 | Absolute display-linear component scale in cd/m^2. | D accepts [0,1]; E accepts [Lb,Lw]. Explicit finite 0<=Lb<Lw; standard exponent fixed at 2.4. |
| pq | Absolute display-linear component scale in cd/m^2. | D accepts [0,1]; E accepts [0,10000]. EOTF/inverse only, fixed 10000 scale, no reference OOTF. |
| hlg_oetf | Normalized scene-linear quantity. | Both directions accept their respective [0,1] inputs. OETF/inverse only; no system gamma, black lift or full display EOTF/OOTF. |
| acescc | Scene-relative scalar transfer, basis stated separately. | Any finite input. Preserve the official nonpositive encoding floor and decoding cap at 65504. |
| acescct | Scene-relative scalar transfer, basis stated separately. | Any finite input. Preserve the negative linear toe and official decoding cap at 65504. |

No output-domain clamp is added. A valid semantic decode can exceed the next semantic encoder's [0,1] input domain in Float64, so that next call is outside its semantic input precondition. A caller needing [0,1] applies an explicit clamp. No exact round-trip promise is attached to such an output.

The signed extension belongs only to gamma/sRGB/709/2020. PQ/HLG semantic domains are caller obligations. ACES intrinsic floors/caps remain in both Float32 and Float64; no unbounded variant is defined under the same identifier. Vendor camera logs require separately versioned definitions. Ordinary log2/log10 math remains in NUM.

## Interface

One input Result with one tensor member produces one `values` output Result containing one tensor. The member is Float32 or Float64; the output retains the dtype, logical shape, axes and channel order. Complete sample rank, including batch and cell axes, is at most 8; sample count is at most 2^40. The operation preserves Result schema identity, tensor key, logical shape and batch axes. It carries no description and sets `atomic_trailing_axes` to zero. No additional resource port, optional data input or runtime-dependent shape exists. No implicit broadcasting, casting, axis movement, range scaling or integer decoding occurs. In semantic mode, non-native numeric storage encoding, including Float32 carrying 0..255 codes, is first converted to the curve's native scalar coordinates through FMT-06. Raw floating calls apply the formula to stored numeric values; integer dtype is unsupported in all modes.

With `input_description`, the described path processes one explicitly selected RGB or Gray group. A Gray group without a channel axis is its one component; an explicit channel axis can occupy any structurally valid position. `axis` indexes a cell axis and excludes the Result batch prefix. The group selector and axis interpretation resolve statically and uniquely. Gray describes the curve-compatible native quantity; normalized CIELAB l, YCbCr or CMYK components do not become linear color by selection. Other groups, alpha and AOVs pass through bit-for-bit. Complete images stay straight with internal alpha.

Raw mode selects ordinary numeric components explicitly: either all components of a tensor, or a nonempty unique static index list on an explicit/resolved axis. A whole single-component plane needs no singleton channel axis. Output shape and dtype are unchanged, and unselected samples copy exactly.

| Parameter | Rule |
| --- | --- |
| input_description | Optional explicit TensorDescription. Presence selects the described RGB/Gray path; absence selects raw numerical components. |
| group | Required static unique RGB/Gray group when selecting an explicit group. An axis-free Gray component may supply its component description without a group. |
| components / axis | Raw all-components or explicit index selection; not combined with semantic group. |
| curve | A may resolve from input_description; an explicit assertion must agree. B and raw require an explicit curve. No default sRGB guess. |
| gamma | Required only for power_gamma; finite static Float64 >0; no implicit 2.2 exponent. |
| coefficient_variant | Only for bt2020: smooth (default), rounded_10bit or rounded_12bit. |
| black_luminance / white_luminance | Required only for bt1886; finite static Float64 Lb,Lw in cd/m^2 with 0<=Lb<Lw. |
| layout | auto by default; view or materialize. |

Irrelevant, contradictory or malformed parameters fail rather than being ignored. When A omits curve, the complete transfer record, including its parameters, comes from input_description. When a curve is explicitly supplied, its resolved defaults and parameters form a full assertion: an explicit bt2020 with omitted variant means smooth, and conflicts with a rounded_10bit source. B's target defaults never overwrite an inconsistent source. Curve, variant and layout are closed String choices; group and raw selectors are typed static selectors with bounded integer indices. The public `TransferDefinition` codec encodes complete transfer identities in bounded `fmt09-v1:` records. Parameter-free curves use their canonical identifier; parameterized records preserve exact Float64 parameter bits. Raw selectors use a bounded canonical index string.

## Curve identity and description rules

The described path resolves compatible global/group/selected-channel assertions, explicit analytic bindings and native numeric encodings. Axis-free Gray groups and component-only Gray descriptions are supported. Gray supplies `coordinates.gray_kind`: `linear_y` for encode or linear decode, and `encoded_luma` for a nonlinear decode. CIELAB/OKLab lightness is not transfer luminance. Canonical `reference=scene|display` requires explicit `coordinates.scale=relative|absolute`; legacy relative/display-absolute reference spellings remain accepted, with contradictory scales rejected. Relative units accept `relative`/`1` (Gray also `relative_luminance`), and absolute display units accept `cd/m2`/`cd/m^2`. Spatial selection agrees with the structural channel axis.

The described path reads only the explicit `input_description`. It requires a unique `group` (or an explicit axis-free Gray component) and rejects `axis/components`. Decode A resolves the complete source transfer when `curve` is omitted; any supplied curve and parameters agree with the description. Encode B always requires a target curve and requires the explicit source transfer to be linear. There is no sRGB guess or automatic scene/display conversion.

Gamma/sRGB preserve the chosen scene-relative or display-relative scale. 709/2020 OETF, HLG OETF and ACES scalar curves require scene-relative quantities; PQ/1886 require absolute display-linear quantities. Curves are independent of RGB primaries: applying an ACEScct scalar curve does not establish a complete ACEScct color space.

Without `input_description`, raw calls require `components` and an explicit curve. An index list requires `axis`; `components=all` may omit it. Raw formula evaluation uses its defined IEEE outcomes, while intrinsic PQ/1886 max and ACES cap/floor branches still apply. Static parameter and storage checks apply in both paths.

Neither path creates a description in the output. A computes the curve's linear quantity and B computes the target encoded quantity; callers provide the corresponding configuration to later consumers.

## Behavior

Every coordinate of the output is initialized: a participating coordinate evaluates its transfer, and an unselected coordinate copies bits. Each output sample depends only on the input sample at the same coordinate (intrinsic floors/caps keep their source dependency because branch selection uses the actual input). A bypass-only request performs no transfer arithmetic. Hidden straight color remains participating when alpha is zero; the curve has no alpha input.

Only linear and gamma=1 are whole-mapping static identities. They copy bits before NaN classification, preserving signaling NaNs and signed zeros. Semantic callers still guarantee finite participating identity samples. A bypass-only request or a runtime cap/floor does not select the identity exception.

`auto` and `materialize` write independent dense output tiles. Forced `view` rejects a statically nonidentity curve during preparation. A nonempty identity `view` returns `InvalidArgument/ViewUnavailable`. Empty requests emit no payload.

| Condition | Phase | Outcome |
| --- | --- | --- |
| Malformed/missing/irrelevant parameter or wrong type, ambiguous selector, source assertion conflict | Static preparation | InvalidArgument / InvalidDomain. |
| Unsupported dtype/model, incompatible units/reference, wrong source transfer or structural layout | Static preparation | TypeMismatch. |
| Semantic nonfinite/out-of-domain input or nonfinite rounded output | Caller sample precondition | No sample error guaranteed. |
| Forced view of a statically nonidentity curve | Static preparation | InvalidArgument / InvalidDomain, ViewUnavailable message. |
| Nonempty identity forced view | Invocation | InvalidArgument / ViewUnavailable. |
| Raw domain/nonfinite/overflow arithmetic | Evaluation | Defined IEEE result of the selected formula. |

## Acceptance

Independent high-precision real-function evaluation and exact rational constants for strict rounding. Reference cases cover every branch threshold and its adjacent Float32/64 values, signed zeros, gamma=1, negative/HDR extensions, nonfinite raw results, valid semantic-domain fixtures, intrinsic plateaus/caps, BT.2020 variants and explicit reference/parameter conflicts. Native identity bit copies and schema-only view failure are verified separately. Semantic fixtures satisfy every participating sample's preconditions; raw fixtures verify NaN payloads, infinities, signed zeros and intrinsic plateaus without a semantic-domain filter. Members add concrete cases.
