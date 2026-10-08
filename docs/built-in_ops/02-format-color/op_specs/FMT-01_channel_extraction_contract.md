---
spec_schema_version: 1
id: FMT-01
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
result_operation_abi: 2
kernel_package: "0.30.0"
---

# FMT-01: extract tensor channels

FMT-01A and FMT-01B are CPU Result operations. FMT-01C is a public authoring
helper that expands a source channel table into independent FMT-01A nodes. The
specification status remains Proposed; implementation and validation status are
separate. All four
installed consumer checks pass: metadata assignment and example, plus channel
extraction and example.

The operations select stored samples without arithmetic, preserving every
sample bit. A selected component can feed an ordinary numeric or image
operation, while other workflow edges continue to reference the original
Result. FMT-03 owns subsets, permutations,
duplicates and replacement; FMT-02 owns explicit channel assembly.

## Result interface and member keys

Each A/B node accepts one Result input named `input` and publishes one Result
output named `values`. Input and output schemas contain one tensor member and no
fields. Package 0.30.0 uses Result operation ABI 2, WorkflowDocument 4 and
OperationTraits 21. The six CPU registry keys are:

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
| `metadata_mode` | `respect`, `raw` or `override`; helpers default to `respect`. Direct nodes provide the mode. |
| `axis` | Nonnegative Int64 cell-axis index. It is required for raw or undescribed data; respect checks it against an attached `channel_axis`. |
| `metadata_override` | Canonical TensorDescription parameter, present exactly in `override` mode. It affects only this invocation. |
| `keepdims` | Bool; false removes the selected cell axis, true keeps it with extent 1. A rank-one cell descriptor requires true. |
| `layout` | `auto`, `view` or `materialize`; helper default is `auto`. |
| `index` (A) | Required nonnegative Int64 less than the selected cell-axis extent. |
| `match`, `selector` (B) | Required static strings. `match` is `name` or `role`; `selector` is exact UTF-8, nonempty and at most 128 bytes. |

B requires a valid TensorDescription with a channel axis and one ordered channel
entry for every index on that axis. It searches the selected name or role field
once at compile time. Matching is case-sensitive and exact, with no aliases,
normalization, trimming or pattern matching. Zero or multiple matches fail.
Named raw selection is undefined and rejected.

The input is a Result with one tensor member and no fields. Static preparation
checks dtype, descriptor rank, selected axis/index, TensorDescription and
`keepdims` shape. Unknown metadata is not guessed into a component description.
Direct A/B nodes may include `expected_source_schema` and
`expected_source_layout` together; preparation compares them to the producer's
schema digest and physical-layout assertion. The schema assertion is the
domain-separated SHA-256 digest of the complete canonical schema, encoded as 64
lowercase hex characters; it covers metadata and batch axes and does not inspect
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
when the selection is a representable channel-plane view. Result metadata
projects the selected component and remaps axis descriptions as defined below.
The extraction does not claim that a component is a complete color or validate
its numeric domain.

Every supported profile preserves stored sample bits exactly, including NaN
payloads, infinities, signed zero, integer extremes and negative/HDR values.
Sample values undergo no floating-point arithmetic, color conversion, cast,
clamp or alpha association change. Projecting TensorDescription metadata may
update an axis origin as `origin += index * step`; that metadata calculation
uses floating-point arithmetic and does not alter sample data.

Extraction has no family-wide product-of-extents limit. It inherits the
requested-region and backing constraints of Result, including sparse domains
whose full sample product overflows `uint64_t`; the focused integration test
extracts a single UInt8 sample at a near-maximum coordinate from such a domain.

## Metadata behavior

`respect` uses an attached `photospider.tensor-description` when valid. An
explicit axis that conflicts with its channel axis fails. `override` validates
and uses the supplied TensorDescription for this node only. `raw` requires an
explicit axis and ignores an attached semantic axis for selection; when a valid
description is available, its still-applicable fields may be projected.

When selecting the described channel axis, the output carries the selected
component's name, role, unit, interpretation, encoding and sampling fields when
they are described. A matching color group's interpretation is projected onto
the component, along with its component encoding and sampling fields when
present; the complete group table is then cleared because one selected
component is not a complete group. A kept singleton axis uses a one-entry
channel table. A removed channel axis retains a component description without
claiming the output still has a channel axis. When selecting another cell axis,
the projector adjusts later channel-axis indices and removes the matching
logical-axis description when that axis is squeezed. For a kept axis, it shifts
the logical coordinate origin by `index * step`. Spatial samples are not
resampled.

The normal extraction path clears input facets and publishes only a projected
TensorDescription facet when one is available. It does not promise to preserve
opaque or unrelated annotations. The operation does not scan sample values,
validate coverage intervals or certify color/alpha semantics.

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

No private worker pool or result cache is used. Views retain source backing and
resource owners for their lifetime. Materialized output owns its copied backing.

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
| Invalid mode, axis, index, `keepdims`, selector, absent or ambiguous name/role | `InvalidArgument` / `InvalidDomain` during static preparation |
| Unsupported element type, malformed Result schema or incompatible descriptor/layout | `TypeMismatch` or the underlying metadata validation status |
| Forced view cannot represent the requested mapping | `InvalidArgument` / `InvalidDomain`, diagnostic `ViewUnavailable` |
| Missing source coverage, work/resource exhaustion, cancellation or upstream failure | Preserve the corresponding Result/runtime status and scope |

 The four installed metadata and extraction consumer checks also pass.
