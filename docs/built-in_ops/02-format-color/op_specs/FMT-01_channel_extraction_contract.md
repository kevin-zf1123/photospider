---
spec_schema_version: 1
id: FMT-01
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
result_operation_abi: 4
kernel_package: "0.35.0"
---

# FMT-01: extract tensor channels

FMT-01A and FMT-01B are CPU Result operations. FMT-01C is a public authoring
helper that expands a source channel table into independent FMT-01A nodes. The
specification status remains Proposed; implementation and validation status are
separate. Interpretation follows [FMT-common](FMT_common_contract.md) C13: an
optional `input_description` parameter supplies channel names, roles and the
channel axis; the input and output Results carry no description.

The operations select stored samples without arithmetic, preserving every
sample bit. A selected component can feed an ordinary numeric or image
operation, while other workflow edges continue to reference the original
Result. FMT-03 owns subsets, permutations,
duplicates and replacement; FMT-02 owns explicit channel assembly.

## Result interface and member keys

Each A/B node accepts one Result input named `input` and publishes one Result
output named `values`. Input and output schemas contain one tensor member and no
fields. Package 0.35.0 uses Result operation ABI 5, WorkflowDocument 6 and
OperationTraits 27. The six CPU registry keys are:

| Member | Strict | Apple Silicon profile | x86-64 profile |
| --- | --- | --- | --- |
| Index (A) | `channel.extract_index_strict` | `channel.extract_index_accelerated_apple_silicon` | `channel.extract_index_accelerated_x86_64` |
| Name/role (B) | `channel.extract_named_strict` | `channel.extract_named_accelerated_apple_silicon` | `channel.extract_named_accelerated_x86_64` |

All three profiles use the same bit-copy algorithm. `strict` is portable;
`accelerated_apple_silicon` and `accelerated_x86_64` require the matching host
capability. No GPU key or fallback is provided.

All seven element types are supported: UInt8, UInt16, Int8, Int16, Int64,
Float32 and Float64. The cell descriptor has rank 1..8 and positive extents.
The complete sample shape, including its batch prefix, has rank at most 8. Each
member preserves all source batch axes. Parameters `axis`, `index`,
and `match` apply to the cell descriptor axes; batch axes form an unchanged
prefix of the complete sample shape.

## Parameters and static checks

Direct registry nodes carry static parameters. The `format` helpers supply
defaults and typed authoring values.

| Parameter | Type and behavior |
| --- | --- |
| `input_description` | Optional canonical `TensorDescription` String (`tensor_description_parameter`). Validated against the input descriptor during preparation. It affects only this invocation. Required for B. |
| `axis` | Optional nonnegative Int64 cell-axis index. Required when no description supplies `channel_axis`; when both are present they must agree. |
| `expected_channels` | Optional positive Int64 assertion of the selected axis extent. |
| `keepdims` | Bool; false removes the selected cell axis, true keeps it with extent 1. A rank-one cell descriptor requires true. |
| `layout` | `auto`, `view` or `materialize`; helper default is `auto`. |
| `index` (A) | Required nonnegative Int64 less than the selected cell-axis extent. |
| `match`, `selector` (B) | Required static strings. `match` is `name` or `role`; `selector` is exact UTF-8, nonempty and at most 128 bytes. |

B requires an `input_description` with a channel axis and one ordered channel
entry for every index on that axis. It searches the selected name or role field
once at compile time. Matching is case-sensitive and exact, with no aliases,
normalization, trimming or pattern matching. Zero or multiple matches fail.
B without `input_description` is rejected.

The input is a Result with one tensor member . Static preparation
checks dtype, descriptor rank, selected axis/index, the supplied description and
`keepdims` shape. Nothing is guessed from the channel count.
Direct A/B nodes may include `expected_source_schema` and
`expected_source_layout` together; preparation compares them to the producer's
schema digest and physical-layout assertion. The schema assertion is the
domain-separated SHA-256 digest of the complete canonical schema, encoded as 64
lowercase hex characters; it covers schema identity and batch axes and does not inspect
samples or certify their validity. Layout has its own assertion. The public
`format::split_channels` helper writes both assertions on every generated A
node. It also verifies a declared input immediately, so forward producer
references are checked when compiled. The physical-layout assertion remains a
separate string parameter capped at 8,192 bytes; an oversized layout assertion
is rejected during authoring.

## Shape and exact sample mapping

Let the source cell shape be `S`, with cell rank `r`, selected cell axis `a`,
and channel index `k`. Let `b` be the number of batch axes. The selected physical
axis in the complete sample shape is `b+a`; batch coordinates pass through
unchanged.

For `keepdims=true`, the output cell shape equals `S` except `O[a]=1`. For
`keepdims=false`, output shape `O` is `S` with axis `a` removed. For every
requested output coordinate `q`, copy exactly the element at source coordinate
`s` defined by:

```text
keepdims=true:  s[b+a]=k; other batch and cell coordinates equal q
keepdims=false: preserve batch coordinates; insert k at cell axis a;
                shift output cell coordinates after a by one source axis
```

Output tensor key, schema id, batch axes and publication policy are retained.
The descriptor reflects the selected shape and its spatial layout remains valid
when the selection is a representable channel-plane view. When `keepdims=false`
removes an axis inside the atomic trailing range, `atomic_trailing_axes`
decreases by one. The output carries no description. The extraction does not
claim that a component is a complete color or validate its numeric domain.

Every supported profile preserves stored sample bits exactly, including NaN
payloads, infinities, signed zero, integer extremes and negative/HDR values.
Sample values undergo no floating-point arithmetic, color conversion, cast,
clamp or alpha association change.

Extraction has no family-wide product-of-extents limit. It inherits the
requested-region and backing constraints of Result, including sparse domains
whose full sample product overflows `uint64_t`.

## Interpretation behavior

With `input_description`, the description's `channel_axis` selects the axis when
`axis` is absent; an explicit `axis` that conflicts with it fails. Without a
description, `axis` is required and only A is available. The description is used
only for selection in this invocation; it is not attached to the output and is
not projected onto it. Spatial samples are not resampled.

A caller that needs a description of the selected component for a later
consumer builds it explicitly, for example with
[`format::edit_description`](FMT-08_metadata_assignment_contract.md) or by
constructing a component `TensorDescription`, and passes it to that consumer.
The operation does not scan sample values, validate coverage intervals or
certify color/alpha semantics.

## Result demand, dirty mapping and empty requests

The output uses Dependency-v2. For output footprint Q, the Result Need requests
exactly Q's mapped source support and the source Descriptor, with role mask 9:
Data (1) plus Descriptor (8). It requests neither Validation nor Control
support. A source change confined to an unselected channel dirties no output
sample. A change in selected-channel support maps to the corresponding output
coordinates. Descriptor changes require reinference.

Empty output demand uses a stateless continuation that publishes an empty Result
without requesting sample payload. An upstream producer may still perform its
own wider work; that producer's demand and failure scope remain unchanged.
Result caching is disabled for A/B.

## Views, copies and ownership

`auto` first tries an authorized Result view and copies only after
`ViewUnavailable`. `view` requires a representable view. `materialize` copies
only requested output coverage through a transactional Result writer. Copy
work checks cancellation within runs of at most 256 samples. Failure or
cancellation publishes no partial output.

For generic tensors, valid positive, negative and zero strides are supported.
The Result mapper can partition a requested window by its authorized backing
regions and retain the corresponding owners, including for a view spanning
multiple backing owners. For spatial tensors, selecting the declared channel
axis can retain the source image owner and preserve the channel plane's source
mapping. Slicing a spatial height or width axis cannot use a forced view: `auto`
materializes in the projected output layout. When squeezing removes a spatial
axis, the output uses a generic tensor layout; with `keepdims=true`, it retains
the spatial layout with a singleton extent. No layout change grants access to
samples outside authorized coverage.

No private worker pool or result cache is used. Views retain source backing
owners for their lifetime. Materialized output owns its copied backing.

## FMT-01C split helper

`format::split_channels` is an authoring helper, not a registry operation. It
receives a workflow document, source edge, inferred single-tensor Result
metadata and `ChannelExtractOptions`. It resolves the cell channel axis and
expands one A node for each channel position. The helper returns
`ChannelHandle{name, output}` entries named `c0` through `c(C-1)`; callers
choose which handles to connect or export. An unrequested sibling does not
request its channel's payload.

The helper validates options and source metadata before appending. For a declared
workflow input, it checks full schema and physical-layout assertions against the
declaration. For a forward producer edge, generated A nodes carry a bounded
SHA-256 digest of the complete canonical schema and a separate physical-layout
assertion for compile-time comparison. It stages all nodes and
handles before changing the document; any error leaves the graph unchanged.
Expansion is bounded to 65,536 channels and also obeys ordinary workflow node
and output limits. No samples are read to determine the channel count.

## Errors and current verification

| Condition | Result |
| --- | --- |
| Missing or conflicting axis, invalid index, `keepdims`, selector, absent or ambiguous name/role, B without `input_description`, failed schema/layout/channel-count assertion | `InvalidArgument` / `InvalidDomain` during static preparation |
| Malformed `input_description` or one that does not fit the input descriptor | The `tensor_description_from_parameter` / `validate_tensor_description` status during static preparation |
| Unsupported element type, malformed Result schema or incompatible descriptor/layout | `TypeMismatch` |
| Forced view cannot represent the requested mapping | `InvalidArgument` / `InvalidDomain`, diagnostic `ViewUnavailable` |
| Missing source coverage, work/resource exhaustion, cancellation or upstream failure | Preserve the corresponding Result/runtime status and scope |
