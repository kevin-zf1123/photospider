---
spec_schema_version: 1
id: FMT-05
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_subset
clarification_status: complete
---

# FMT-05: internal alpha editing, extraction and removal

FMT-05 remains Proposed. The installed `photospider/ops/format/alpha.hpp` exposes
`format::set_alpha`, `format::extract_alpha`, and `format::remove_alpha`.
`set_alpha` appends the registered `alpha.set_<profile>` Result operation;
B/C compile into compositions of registered channel extraction, literal-like
fill, mapped assembly and core identity operations. FMT-04 association
operations are also registered. The profiles are `strict`,
`accelerated_apple_silicon`, and `accelerated_x86_64`.

All FMT-05B/C inputs and outputs use the single-tensor Result ABI with
support the seven native copy dtypes, preserve the batch prefix, and avoid
sample-domain validation. Axes are cell-relative and exclude the Result batch
prefix. The complete sample rank, including batch and cell axes, is at most 8;
full sample count is at most 2^40. Native FMT-04/FMT-05A operations use fixed invocation ABI 8. B/C are
compile-time compositions over registered Result operations.

Inherit [FMT-common](FMT_common_contract.md), its NUM baseline and the
[kernel storage contract](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
The canonical straight-image contract uses internal alpha references and keeps
external sources as explicit graph inputs.
FMT-05A uses `alpha.set_<profile>`.
Members are [A set](FMT-05A_set_alpha.md), [B extract](FMT-05B_extract_alpha.md)
and [C remove](FMT-05C_remove_alpha.md).

FMT-05A declares only each output slot's mapped source payload. Copied colors
have no additional alpha-validation dependency. Every connected input retains
static schema checks. Set writes independent dense output tiles with zero scratch;
nonempty view tasks fail ViewUnavailable with schema-only dependencies. Empty
has no payload task. A channel axis contains at most 65,536 source slots.

## Purpose and representation

A sets/adds alpha, B extracts alpha and C removes alpha for one explicitly
selected color group. Complete images use straight color coordinates, including
RGB, Gray and non-RGB models. Alpha is an independent plane in the same tensor,
identified by the required explicit `input_description`, never by channel count.
No operation composites a background, transforms a color model or changes
transfer, numeric encoding or spatial sampling implicitly.

External alpha planes and explicit scalars remain valid inputs to A, but become
internal result channels. There is no external placement option or persistent
cross-tensor alpha binding. B produces a standalone alpha component tensor;
that result does not remain semantically attached to the source image.
Ordinary input/view lifetime ownership still follows the kernel contract.

A and C preserve straight color samples bit-for-bit.
Neither setting alpha=0 nor removing alpha erases finite hidden straight color.
Premultiplied boundary payloads must first be explicitly converted to canonical
straight images; this family does not silently accept or normalize them.

## Confirmed decisions

| Topic | Selected contract |
| --- | --- |
| Members | A set/add; B extract; C remove; one group per invocation. |
| Identity | A uses the registered `alpha.set_<profile>` Result operation. B/C are executable compile-time compositions of registered channel operations, constants and core identity. |
| Representation | Canonical straight image, with any alpha in the same tensor. No external persistent binding. |
| A source | Same-tensor selected channel, matching explicit external component plane, or explicit shape-[1] scalar. No implicit broadcast of a plane. |
| A placement | Preserve an existing alpha position by default; otherwise explicit channel position. New alpha has no guessed last-channel placement. |
| A color | Copy stored straight color; no multiplication/division and no zero-alpha clearing. |
| A caller premises | Selected represented colors are finite and new alpha is finite in [0,1]. These semantic obligations create no validation-only payload edges. |
| B absence | missing_alpha=error by default; explicit opaque generates the legal exact code for decoded coverage one. |
| B samples | Copy actual alpha bits without color reads or an added finite/range scan. |
| B shape | Complete nonchannel shape S; keepdims=false by default, optional singleton original channel axis; no rank-zero result. |
| C absence | missing_alpha=error by default; explicit identity returns unchanged data. |
| C color | Copy stored straight color; no restoration division, hidden-color loss or background blending. |
| Shared alpha | Reject A's in-place overwrite of an alpha slot referenced by other groups; use a new channel. C keeps the shared sample slot; callers configure the selected group downstream. |
| Old alpha cleanup | Remove an old internal alpha channel only when no other declared reference remains; retain shared channels. |
| Target index | A's channel_index is a final output position, never a request to overwrite color/AOV data. |
| Layout | auto/materialize produce independent output tiles; nonempty view fails with schema-only dependencies. The axis-free C identity branch retains its stated helper exception. |
| Dtypes | A uses Float32/Float64 with same-dtype alpha. B/C preserve existing dtype. B opaque uses explicit alpha_encoding or FMT-06 dtype-default encoding, without a dependency on retired FMT-07. |

## Common interface and support

A has `input`, optional explicit `alpha`, and one `values` output. Its helper is
available and lowers to the registered `alpha.set_<profile>` operation. B/C are
public compile-time helpers taking a graph, a single-tensor `input` Result edge
and inferred structural `OperationMetadata`. B returns `values`; C returns
`values` for mapped assembly and `value` for axis-free `core.identity`. Their Result
paths are executable through existing registered operations. All outputs
preserve input dtype. A accepts Float32/Float64, with external
alpha/scalar matching exactly; it interprets alpha directly in [0,1]. Integer
codes require explicit decoding/casting before A. B/C copy existing samples of
UInt8, UInt16, Int8, Int16, Int64, Float32 or Float64. Integer-coded A inputs require explicit numeric decoding before use. No implicit precision promotion or range conversion occurs.

B opaque generates a same-dtype code that decodes exactly to mathematical
coverage one. Optional static alpha_encoding specifies the fallback output's
numeric encoding and legal code domain. When omitted, use FMT-06's dtype-default
encoding of coverage [0,1]: Float32/64 one, UInt8 255, UInt16 65535, Int8 127,
Int16 32767 or Int64 2^63-1. An explicit 10-bit encoding in UInt16 yields 1023;
an explicit reversed encoding can yield zero. Do not infer the new alpha encoding
from another color channel. The chosen code must be in its legal domain and
exactly representable in output dtype; otherwise fail before pixel generation.
Do not quantize an almost-opaque substitute or silently clamp it.

The alpha_encoding parameter describes the missing-alpha fallback only. It is
allowed with missing_alpha=opaque, structurally validated, and never re-encodes
an alpha channel that actually exists. Generated output carries no encoding
or coverage description. Callers supply the encoding to downstream consumers.
Naming a container dtype does not infer reduced bit depth. Existing integer
alpha extraction and removal remain bit copies without decode/re-encode.

| Common static parameter | Contract |
| --- | --- |
| group | Required explicit group identity; exact unique resolution in input_description. Do not infer a group from channel count. |
| input_description | Required explicit source interpretation, validated against the input structure. Raw channel edits use FMT-01/02/03. |
| axis | Optional Int64 assertion of the effective cell-channel axis, nonnegative and within cell rank; excludes the batch prefix and is absent for a component Gray input. |
| layout | auto by default; view/materialize explicit, with the rules below. |
| profile | B/C helper selection: strict by default, accelerated_apple_silicon or accelerated_x86_64. A selects the corresponding proposed primitive key. |

All structural parameters and group/channel maps are static. Samples may differ
between executions. Cell axes are indexed relative to the Result batch prefix. The complete sample
rank, including batch and cell axes, stays at most 8; full sample count is at
most 2^40 with checked positive extents. When insertion adds a cell axis, the full input sample rank must be at most 7
so the output rank remains at most 8. The same
spatial sample grid survives channel surgery. External inputs are matched by
shape and dtype; no attached coordinate labels are compared or propagated. Strict and
accelerated outputs are bit-identical; predicates, metadata and failures remain
exact, with no floating tolerance. CPU strict/named profiles are the target;
GPU and cross-backend conformance are not claimed.

## Structure and channel surgery

Resolve the selected group and input channel axis from the explicit
`input_description`. Let S be the shape after erasing
the channel axis, or the whole shape of a component Gray input. Group selectors,
axis choices, placements and channel maps are static; sample values may vary
between executions. Spatial planes must match S and the batch prefix;
there is no implicit squeeze, resize or spatial broadcast. Scalar is an explicit
source kind, even when a plane happens to have shape [1].

For A, an existing private alpha slot can be replaced at its current position.
When explicitly moving/adding alpha, first remove the old slot if it becomes
unreferenced, then insert a new alpha slot at the stated final index, shifting
other channels in order. Retain an old slot that is still referenced. Reusing
the shared slot as an in-place edit is rejected rather than silently modifying
other groups. Component Gray needs an explicit new output channel axis when
adding alpha. The result contains actual new-alpha samples, including expanded
scalar samples, in its logical alpha plane.

For B, return S by default. If the primary image has a channel axis,
keepdims=true replaces that axis extent by one. Erasing the sole axis of a
rank-one channel vector would produce rank zero, so require keepdims=true.
A component input with no channel axis retains its original nonchannel shape;
keepdims cannot invent an axis. Missing-alpha opaque generation uses this same
output shape, not the shape of a scalar constant source.

For C, inspect alpha references in the explicit input configuration and delete
the alpha sample channel only when no other group uses it. Preserve shared
channels, all surviving sample bits and an existing channel axis even if one
channel remains. The helper does not edit descriptions: callers construct the
selected group's downstream configuration explicitly.

Every channel map reads original immutable inputs. Static shape/description
checks apply to all required ports even when samples are not requested. Target
assignment cannot change the mandatory straight image representation or create
an external alpha binding. Raw structural editing remains available through
FMT-01/02/03 and ordinary NUM operations; it does not establish FMT-05's semantic caller premises or another canonical image state.

## Fixed tile dependencies and layout

| Output value | Required source |
| --- | --- |
| A new alpha | Mapped alpha, including scalar index zero when needed. |
| A selected color or unrelated channel | Its original mapped component; no additional new-alpha read. |
| B actual alpha | Selected internal alpha sample bits. |
| B opaque fallback | Static exact opaque code and schema only. |
| C surviving channel | Its original mapped component. |

The compiler retains complete output tiles, maps each intersecting channel span,
and unions closures in each source's own tile grid. It preserves gaps and declares
schema-only dependencies for unused ports. Logical per-value formulas do not
promise sample-granular scheduling. A tile containing both color and new alpha
needs both sources; a color-only tile can omit the alpha producer. Static
parameters and descriptions are validated even for Empty.

FMT-05A auto/materialize copy to distinct dense row-major output allocations,
including identity maps. Every nonempty view task fails
`InvalidArgument / ViewUnavailable` with only schema dependencies. Set declares
zero scratch. Public visibility follows successful ordered call retirement.

B uses fixed CPU tasks for both existing-alpha extraction and opaque generation. Auto/materialize produce independently owned dense output tiles; every nonempty view task fails InvalidArgument/InvalidDomain with schema-only dependency and no source payload read. Empty still validates static parameters and emits no task. Channel-bearing C uses fixed mapped assembly: auto/materialize copy, and nonempty view fails InvalidArgument/ViewUnavailable with schema-only dependencies. C component Gray identity uses core.identity.value and explicit COPY tasks, preserving sample bits without applying the helper layout policy.

## Ownership and numerical premises

External/scalar sources become ordinary samples in independently owned output
tiles. Spatial metadata describes logical axes, independently of physical dense
strides. Retained Results survive input and context destruction; no hidden alpha
snapshot or metadata relation supplies missing coverage.

Set callers supply finite selected color values and finite normalized alpha for
the semantic values they represent. These premises apply to the complete retained
output tiles and promise no runtime validation or coordinate diagnostic. Source
tile over-grants do not themselves evaluate other components. B/C retain their
bit-copy/fill semantics without adding these Set premises.

Static group, shape, reference, placement and representation conflicts fail
preparation. Resources, cancellation, missing bindings and producer failures
follow the fixed executor protocol. A byte copy does not certify other semantic
values or make an unproduced alpha tile available.

## Required semantic acceptance cases

Required independent cases include:

- Straight C=0.5 with old alpha=0.5 and new alpha=0.25 gives C=0.5, alpha=0.25.
  New alpha=0 still gives C=0.5. Removing alpha preserves C=0.5.
- A preserves finite hidden straight color at new alpha=0. Its selected-color
  finiteness is a caller premise; unrelated AOV copies add no such premise.
- B preserves negative zero, out-of-range samples and NaN payload bits; opaque
  fallback generates the exact encoded coverage-one code over S. No source color read occurs.
- A shared-slot replacement fails; explicit insertion retains the old shared
  slot. C keeps the shared channel; downstream group configuration is explicit.
- Moving a private alpha removes its former slot, inserts at the final specified
  index and preserves every other channel's order and exact values.
- R-only A output does not certify or secretly supply unproduced alpha. A later
  alpha request obtains the declared output channel through normal execution.
- External/scalar inputs never leave persistent alpha-source references in
  the result. Output interpretation must be supplied explicitly to later consumers.

## Reference execution, resources and composition

Static preparation resolves group, description and channel surgery, then retains
immutable source maps. Fixed kernels copy or fill all coordinates in each
retained output tile. Original inputs supply every source, so channel movement
cannot become a sequential overwrite. Set polls cancellation every 128 samples,
uses no arithmetic scratch and creates no dynamic windows or payload allocations.
Prepared maps and read indexes support concurrent Runs; the executor owns workers,
output reservations, completion and ordered publication.

B resolves its opaque constant once with exact descriptor arithmetic, without
source samples; include descriptor/encoding work in compile/admission budgets.
Construct Int64 endpoints directly, never through Float64.

B/C stage their helper expansion and handles transactionally, using the existing
collision-aware authoring ID rules. Errors leave the caller graph unchanged.
Lower B to exact FMT-01A extraction or literal-like fill with the exact opaque
code and structural output shape. Lower channel-bearing C to mapped assembly;
axis-free C identity uses `core.identity` with output `value`. That identity
branch adds no source-schema assertion or layout conversion. Mapped assembly
uses the aggregate structural `expected_inputs` assertion. These helpers add
no sample checks from A and no persistent alpha relation.

## Error phases and diagnostics

| Condition | Phase | Status / reason |
| --- | --- | --- |
| Missing/ambiguous group or selector, invalid axis/placement, overlapping color and target alpha roles, shared-slot overwrite, malformed input_description | Compile/direct preflight | InvalidArgument / InvalidDomain. |
| Missing alpha without the selected fallback | Static preflight | InvalidArgument / InvalidDomain; identify selected group. |
| Unsupported dtype, mixed A dtypes, nonchannel shape mismatch or noncanonical premultiplied input | Static preflight | TypeMismatch / None. |
| Invalid opaque encoding, or coverage one has no legal exactly representable output code | Static preflight | InvalidArgument / InvalidDomain. |
| Nonempty forced view | Invocation | InvalidArgument; reason follows the selected fixed primitive. Set and mapped removal use ViewUnavailable. |
| Size/offset overflow, budget/work, missing source coverage, backend, cancellation or stale failure | Inherited phase | Preserve NUM/kernel code, origin and scope. |

Static errors carry no invented sample coordinate. Invalid Set numerical
premises have no promised runtime diagnostic. B/C add no NaN/Inf or coverage-range
checks for copies. Missing source coverage remains an ordinary binding failure.

## Validation and performance evidence

Temporary public workflow checks should inspect output samples, structural
schemas, fixed per-port tile reads, exact opaque encodings and failed-expansion
rollback for A/B/C. Numerical alpha oracles remain independent references;
unit arithmetic alone does not prove workflow behavior. Delete temporary
numeric/integration checks after validation. No installed-consumer or current
performance result is asserted here.
