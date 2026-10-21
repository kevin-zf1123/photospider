---
spec_schema_version: 1
id: FMT-01
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
fixed_invocation_abi: 8
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
fields. The six fixed invocation ABI 8 CPU registry keys are:

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
schema digest and spatial-topology assertion. The schema assertion is the
domain-separated SHA-256 digest of the complete canonical schema, encoded as 64
lowercase hex characters; it covers schema identity and batch axes and does not inspect
samples or certify their validity. The separate layout assertion encodes spatial height/width/channel axes; it does not assert runtime strides, offsets or row pitch. The physical Plan and Run binding validate those views. The public
`format::split_channels` helper writes both assertions on every generated A
node. It also verifies a declared input immediately, so forward producer
references are checked when compiled. The spatial-topology assertion remains a
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
when the selected axis is the declared channel axis or keepdims retains a singleton spatial axis. When `keepdims=false`
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

## Fixed execution, input closure and Empty

Each selected output tile is one CPU Host task with one preallocated dense output and zero scratch. Lowering maps that tile's global coordinates to the source thin rectangle defined above, then declares the intersecting tiles from the source's own grid. Batch, atomic or spatial axes may enlarge physical input coverage. The kernel reads only the selected coordinates within those declared views; a required upstream producer still computes its own declared tiles. Prepared input slots are immutable and support legal signed/zero strides and unaligned offsets. DenseHost input adaptation uses explicit Plan COPY tasks.

Static preparation validates selectors, source assertions and the output schema. Kernel preparation verifies the symbol/contract, exact input slots, output geometry, empty task parameters and zero scratch. Runtime performs bitwise copies into the Plan's dense output and checks cancellation between at most 256 samples. Scratch and coordinate state are independent of the full logical extent; input descriptors and output allocations scale with the selected tile closure.

Empty requests retain static validation and emit no payload task. Independent Runs share immutable preparation and have distinct output storage. Selection and imported Plan artifacts preserve the declared closure; execution does not discover further dependencies. Incremental dirty propagation, content caching and source alias identities are outside this fixed execution contract.

## Layout policy, ownership and failures

`auto` and `materialize` both copy selected output tiles into independent storage. The fixed Plan requires each produced tile to own a distinct allocation, so every nonempty `view` task returns InvalidArgument/InvalidDomain with `ViewUnavailable`. Such a task depends only on the source schema and reads no payload, so it does not trigger an otherwise unused source producer. Empty view requests succeed after static checks. A mandatory source effect or other required computation retains its own scheduling and failure obligations.

Selecting the declared spatial channel axis preserves the projected spatial topology. Squeezing a height/width or another nonchannel axis produces generic topology; keepdims preserves the singleton spatial extent. These topology rules do not alter the independent dense output allocation. The output has no attached interpretation.

Completed tiles can satisfy internal dependencies. Public call results appear only after every required task succeeds and the call retires in order; failures and cancellation expose no partial call result. Retained copied outputs survive source, Run and context teardown. There is no private worker pool or dynamic publication callback.

## FMT-01C split helper

`format::split_channels` is an authoring helper, not a registry operation. It
receives a workflow document, source edge, inferred single-tensor Result
metadata and `ChannelExtractOptions`. It resolves the cell channel axis and
expands one A node for each channel position. The helper returns
`ChannelHandle{name, output}` entries named `c0` through `c(C-1)`; callers
choose which handles to connect or export. An unrequested sibling does not
request its channel's payload.

The helper validates options and source metadata before appending. For a declared
workflow input, it checks full schema and spatial-topology assertions against the
declaration. For a forward producer edge, generated A nodes carry a bounded
SHA-256 digest of the complete canonical schema and a separate spatial-topology
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
| Any nonempty forced-view selection | `InvalidArgument` / `InvalidDomain`, diagnostic `ViewUnavailable` |
| Missing source coverage, work/resource exhaustion, cancellation or upstream failure | Preserve the corresponding Result/runtime status and scope |

For temporary public checks, compare raw bytes for all seven dtypes, named/index equivalence, every cell-axis position, batch/spatial/atomic projection, sparse large logical domains, signed/zero-stride inputs, Empty and all layout policies. Exercise split and existing-alpha extraction through Compiler/ExecutionContext, verify malformed signatures and source assertions, and check concurrent Runs, cancellation, independent result owners and failure publication. Retain the independent byte oracle; delete temporary builtin test programs after validation.
