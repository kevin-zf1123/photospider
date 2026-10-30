---
spec_schema_version: 1
id: FMT-05
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# FMT-05: internal alpha editing, extraction and removal

`photospider/ops/format/alpha.hpp` exposes `format::set_alpha`, `format::extract_alpha`, and `format::remove_alpha`. `set_alpha` appends the registered `alpha.set_<profile>` Result operation; B/C compile into compositions of registered channel extraction, literal-like fill, mapped assembly and core identity operations. The profiles are `strict`, `accelerated_apple_silicon`, and `accelerated_x86_64`.

All FMT-05B/C inputs and outputs use the single-tensor Result form, support the seven native copy dtypes, preserve the batch prefix, and perform no sample-domain validation. Axes are cell-relative and exclude the Result batch prefix. The complete sample rank, including batch and cell axes, is at most 8; full sample count is at most 2^40.

Inherits [FMT-common](FMT_common_contract.md) and its NUM baseline. Members are [A set](FMT-05A_set_alpha.md), [B extract](FMT-05B_extract_alpha.md) and [C remove](FMT-05C_remove_alpha.md). Each connected input retains static schema checks. A channel axis contains at most 65,536 source slots.

## Purpose and representation

A sets/adds alpha, B extracts alpha and C removes alpha for one explicitly selected color group. Complete images use straight color coordinates, including RGB, Gray and non-RGB models. Alpha is an independent plane in the same tensor, identified by the required explicit `input_description`, never by channel count. No operation composites a background, transforms a color model or changes transfer, numeric encoding or spatial sampling.

External alpha planes and explicit scalars are valid inputs to A, but become internal result channels. There is no external placement option or persistent cross-tensor alpha binding. B produces a standalone alpha component tensor that is not semantically attached to the source image.

A and C preserve straight color samples bit-for-bit. Neither setting alpha=0 nor removing alpha erases finite hidden straight color. Premultiplied boundary payloads must first be explicitly converted to straight images; this family does not accept or normalize them.

## Family decisions

| Topic | Selected contract |
| --- | --- |
| Members | A set/add; B extract; C remove; one group per invocation. |
| Identity | A uses the registered `alpha.set_<profile>` Result operation. B/C are compile-time compositions of registered channel operations, constants and core identity. |
| Representation | Straight image, with any alpha in the same tensor. No external persistent binding. |
| A source | Same-tensor selected channel, matching explicit external component plane, or explicit shape-[1] scalar. No implicit broadcast of a plane. |
| A placement | Preserve an existing alpha position by default; otherwise explicit channel position. New alpha has no guessed last-channel placement. |
| A color | Copy stored straight color; no multiplication/division and no zero-alpha clearing. |
| A caller premises | Selected represented colors are finite and new alpha is finite in [0,1]. |
| B absence | missing_alpha=error by default; explicit opaque generates the legal exact code for decoded coverage one. |
| B samples | Copy actual alpha bits without color reads or a finite/range scan. |
| B shape | Complete nonchannel shape S; keepdims=false by default, optional singleton original channel axis; no rank-zero result. |
| C absence | missing_alpha=error by default; explicit identity returns unchanged data. |
| C color | Copy stored straight color; no restoration division, hidden-color loss or background blending. |
| Shared alpha | A's in-place overwrite of an alpha slot referenced by other groups is rejected; use a new channel. C keeps the shared sample slot. |
| Old alpha cleanup | Remove an old internal alpha channel only when no other declared reference remains; shared channels are retained. |
| Target index | A's channel_index is a final output position, never a request to overwrite color/AOV data. |
| Layout | auto/materialize produce independent output tiles; nonempty view fails. The axis-free C identity branch is an exception (see C). |
| Dtypes | A uses Float32/Float64 with same-dtype alpha. B/C preserve existing dtype. B opaque uses explicit alpha_encoding or FMT-06 dtype-default encoding. |

## Interface

A has `input`, optional explicit `alpha`, and one `values` output; it lowers to the registered `alpha.set_<profile>` operation. B/C are public compile-time helpers taking a graph, a single-tensor `input` Result edge and inferred structural `OperationMetadata`. B returns `values`; C returns `values` for mapped assembly and `value` for axis-free `core.identity`. All outputs preserve input dtype. A accepts Float32/Float64, with external alpha/scalar matching exactly; it interprets alpha directly in [0,1]. Integer-coded A inputs require explicit numeric decoding before use. B/C copy existing samples of UInt8, UInt16, Int8, Int16, Int64, Float32 or Float64. No implicit precision promotion or range conversion occurs.

B opaque generates a same-dtype code that decodes exactly to mathematical coverage one. Optional static alpha_encoding specifies the fallback output's numeric encoding and legal code domain. When omitted, FMT-06's dtype-default encoding of coverage [0,1] applies: Float32/64 one, UInt8 255, UInt16 65535, Int8 127, Int16 32767 or Int64 2^63-1. An explicit 10-bit encoding in UInt16 yields 1023; an explicit reversed encoding can yield zero. The new alpha encoding is never inferred from another color channel. The chosen code must be in its legal domain and exactly representable in the output dtype (an unchecked caller premise); an almost-opaque substitute is not quantized and nothing is clamped.

The alpha_encoding parameter describes the missing-alpha fallback only. It is allowed with missing_alpha=opaque, is structurally validated, and never re-encodes an alpha channel that exists. Generated output carries no encoding or coverage description. Naming a container dtype does not infer reduced bit depth. Existing integer alpha extraction and removal are bit copies without decode/re-encode.

| Common static parameter | Contract |
| --- | --- |
| group | Required explicit group identity; exact unique resolution in input_description. A group is never inferred from channel count. |
| input_description | Required explicit source interpretation, validated against the input structure. Raw channel edits use FMT-01/02/03. |
| axis | Optional Int64 assertion of the effective cell-channel axis, nonnegative and within cell rank; excludes the batch prefix and is absent for a component Gray input. |
| layout | auto by default; view/materialize explicit. |
| profile | B/C helper selection: strict by default, accelerated_apple_silicon or accelerated_x86_64. A selects the corresponding primitive key. |

All structural parameters and group/channel maps are static. Samples may differ between executions. When insertion adds a cell axis, the full input sample rank is at most 7 so the output rank remains at most 8. The same spatial sample grid survives channel surgery. External inputs are matched by shape and dtype. Strict and accelerated outputs are bit-identical, with no floating tolerance.

## Structure and channel surgery

The selected group and input channel axis are resolved from the explicit `input_description`. Let S be the shape after erasing the channel axis, or the whole shape of a component Gray input. Spatial planes match S and the batch prefix; there is no squeeze, resize or spatial broadcast. Scalar is an explicit source kind, even when a plane happens to have shape [1].

For A, an existing private alpha slot can be replaced at its current position. When explicitly moving/adding alpha, the old slot is first removed if it becomes unreferenced, then a new alpha slot is inserted at the stated final index, shifting other channels in order. An old slot that is still referenced is retained. Reusing the shared slot as an in-place edit is rejected. Component Gray needs an explicit new output channel axis when adding alpha. The result contains actual new-alpha samples, including expanded scalar samples, in its logical alpha plane.

For B, S is returned by default. If the primary image has a channel axis, keepdims=true replaces that axis extent by one. A rank-one channel vector requires keepdims=true (erasing the sole axis would produce rank zero). A component input with no channel axis retains its original nonchannel shape; keepdims cannot invent an axis. Missing-alpha opaque generation uses this same output shape.

For C, alpha references in the explicit input configuration are inspected and the alpha sample channel is deleted only when no other group uses it. Shared channels, all surviving sample bits and an existing channel axis (even if one channel remains) are preserved. The helper does not edit descriptions.

Every channel map reads the original inputs, so channel movement is never a sequential overwrite. Static shape/description checks apply to all required ports even when samples are not requested.

## Behavior

| Output value | Required source |
| --- | --- |
| A new alpha | Mapped alpha, including scalar index zero when needed. |
| A selected color or unrelated channel | Its original mapped component; no new-alpha read. |
| B actual alpha | Selected internal alpha sample bits. |
| B opaque fallback | Static exact opaque code and schema only. |
| C surviving channel | Its original mapped component. |

FMT-05A auto/materialize copy to distinct dense output, including identity maps. Every nonempty view request fails `InvalidArgument / ViewUnavailable`. B uses auto/materialize for both existing-alpha extraction and opaque generation; a nonempty view fails `InvalidArgument / InvalidDomain`. Empty validates static parameters and emits no payload. Channel-bearing C uses mapped assembly: auto/materialize copy, nonempty view fails `InvalidArgument / ViewUnavailable`. C component Gray identity uses `core.identity.value` with explicit COPY, preserving sample bits without applying the helper layout policy.

Set callers supply finite selected color values and finite normalized alpha for the semantic values they represent; no runtime validation or coordinate diagnostic is provided. B/C retain bit-copy/fill semantics without these premises. Retained Results do not depend on a hidden alpha relation; an R-only A output does not supply unproduced alpha.

## Errors

| Condition | Phase | Status / reason |
| --- | --- | --- |
| Missing/ambiguous group or selector, invalid axis/placement, overlapping color and target alpha roles, shared-slot overwrite, malformed input_description | Preflight | InvalidArgument / InvalidDomain. |
| Missing alpha without the selected fallback | Preflight | InvalidArgument / InvalidDomain; identify selected group. |
| Unsupported dtype, mixed A dtypes, nonchannel shape mismatch or noncanonical premultiplied input | Preflight | TypeMismatch / None. |
| Malformed opaque-encoding syntax/type/structure | Preflight | InvalidArgument / InvalidDomain. |
| Coverage one has no legal exactly representable output code | Outside the numerical caller premises | No validation or diagnostic guarantee. |
| Nonempty forced view | Invocation | InvalidArgument; Set and mapped removal use ViewUnavailable. |

B/C add no NaN/Inf or coverage-range checks for copies.

## Lowering

B resolves its opaque constant once with exact descriptor arithmetic, without source samples; Int64 endpoints are constructed directly, never through Float64. B/C stage their helper expansion and handles transactionally with collision-aware IDs; errors leave the caller graph unchanged. B lowers to FMT-01A extraction or literal-like fill with the exact opaque code and structural output shape. Channel-bearing C lowers to mapped assembly; axis-free C identity uses `core.identity` with output `value` and adds no source-schema assertion or layout conversion. Mapped assembly uses the aggregate structural `expected_inputs` assertion.

## Acceptance

- Straight C=0.5 with old alpha=0.5 and new alpha=0.25 gives C=0.5, alpha=0.25. New alpha=0 still gives C=0.5. Removing alpha preserves C=0.5.
- A preserves finite hidden straight color at new alpha=0.
- B preserves negative zero, out-of-range samples and NaN payload bits; opaque fallback generates the exact encoded coverage-one code over S with no source color read.
- A shared-slot replacement fails; explicit insertion retains the old shared slot. C keeps the shared channel.
- Moving a private alpha removes its former slot, inserts at the final specified index and preserves every other channel's order and exact values.
- External/scalar inputs never leave persistent alpha-source references in the result.
