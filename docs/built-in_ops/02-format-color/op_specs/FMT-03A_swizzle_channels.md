---
spec_schema_version: 1
id: FMT-03A
parent_id: FMT-03
function: swizzle_channels
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-03A: reorder, select and fill channel slots

Runtime update: FMT-03A/B are implemented as transactional public authoring
helpers over FMT-02C. Scalar fills are fused into its internal mapped dependency
plan under the fusion permission below; ordinary FMT-02C authoring still accepts
only component/channel sources. Typed literals use exact-bit scalar providers.
See [implementation and runnable workflow](../../../kernel-architecture/Channel-and-Color-Operations.md#fmt-03-channel-editing)
and the [measured CPU performance](../../../../examples/channel_editing_performance/README.md).
Proposed remains the specification decision status.

Inherit the complete [FMT-03 family contract](FMT-03_channel_editing_contract.md),
including its support matrix through FMT-02, resource/error rules and scalar-fill
lowering. The proposed authoring helper `swizzle_channels` receives a graph,
`base` edge, any connected scalar edges, static slot expressions and the family
parameters. It returns one `values` edge handle. It is not a new native registry
key or an alias of the old `channel.swizzle` operation.

## Interface and meaning

Resolve base channel axis a through metadata or explicit `axis`. The static
ordered `slots` list contains at least one entry; each entry is a unique exact
base index/name/role selection or a reference to a same-dtype shape-[1] scalar.
The word unique applies to name/role resolution, not to repeated selections:
several output slots may read the same base channel or scalar. A typed literal
is an authoring shortcut for an equivalent scalar source. Raw uses explicit
axis/index selections; literals and connected scalars preserve all sample bits.

For base rank r and shape S with its channel axis removed, output rank remains r
and shape is insert(S,a,len(slots)). No spatial dimension or channel-axis position
changes. Do not implicitly keep alpha: it must appear in slots when desired.
No zero-channel result, implicit constant, squeeze or broadcast is introduced.

Each output coordinate j selects slots[j[a]]. A base index s reads
base[insert(erase(j,a),a,s)]; a scalar reads V[0]. These are exact byte copies,
including HDR/negative values, signed zeros, infinities and NaN payloads. No
color conversion, alpha reassociation or numerical interpretation is applied.

## Metadata and graph expansion

Default component descriptions follow base selections. Retain a complete group
only with the family's unique remapping of all required members and companions;
otherwise retain applicable component meaning and omit the incomplete group.
An unlabelled scalar source does not become alpha merely by occupying slot 3.
Explicit output component/group definitions authorize target reinterpretation,
including Gray/Black-White uses, without source mutation or extra raw/override.

Lower to one FMT-02C mapping row per output slot and any required generic scalar
fill sources. Compute the projected output metadata explicitly, retain the base
Descriptor dependency even for all-constant output, and use the family's atomic
graph-expansion, profile, scalar rank-one and layout rules. No native output port
or runtime tensor collection is created by the helper itself.

## Exact observation and ownership

For requested footprint Q, source Data is the exact union selected by the slot
expressions. Requesting only a scalar-backed slot reads the scalar and no base
pixels. Repeated base selection is deduplicated in input support, while every
requested destination is still produced. A source change dirties all selecting
output slots in the affected nonchannel coordinates. A scalar change dirties
all observed samples of its selecting slots and no other output slots.

Do not widen reads to complete colors, planes or tiles. Inherit required upstream
failures. Output view availability follows the actual mapping; a reorder or
constant insertion may require materialization. Constant-only images still use
canonical planar page-backed storage when materialized. Output owner retention,
copy/fill complexity, cancellation, errors and performance cases are those of
the family and expanded graph, not those of the retired Whole implementation.

## Independent acceptance fixtures

Use Float32 base [1,2,4] with straight RGB and independent alpha descriptions:

```
[[[10,20,30,0.4], [11,21,31,0.6]]]
```

Slots [2,1,0,3] give `[30,20,10,0.4]` and `[31,21,11,0.6]` at the two pixels.
Default slot roles are B/G/R/alpha, with RGB role-to-slot references R=2, G=1,
B=0. Requesting output (0,1,0) reads only base (0,1,2)=31. A base R-only change
does not dirty this output observation. An explicit canonical R/G/B target
description changes output interpretation but leaves those copied bytes intact.

Slots [0,0,constant(V)] with Float32 V=[0.5] produce [10,10,0.5] and
[11,11,0.5]. Duplicate R selections do not implicitly form an RGB group. A
request for only slot 2 reads V[0] without base pixels. Changing V to -0.0 or
a Float32 NaN with payload 0x7fc01234 changes only that slot's output bits, with
no shape or mapping change. Use byte comparison, not floating equality.

Identity slots [0,1,2,3] preserve samples and applicable descriptions; explicit
materialize still requests independent backing. Also cover one slot, all
constant slots, rank-one and rank-eight bases, arbitrary channel axes, name/role
ambiguity, literal/scalar dtype mismatch, and invalid/empty selections.

The independent oracle enumerates output coordinates and copies bytes from an
independently evaluated source expression; separately enumerate its Data and
dirty sets. Validate lowered graphs against these results. The conceptual public
workflow is `base + scalar -> swizzle_channels expansion -> named values`.
Implementation must provide actual compile/execute commands, a partial channel
request and checked output. Runtime and benchmark evidence is linked above.
