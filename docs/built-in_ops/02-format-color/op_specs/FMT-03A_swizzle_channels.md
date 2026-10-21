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
---

# FMT-03A: reorder, select and fill channel slots

Runtime contract: this installed helper expands transactionally into the
registered FMT-02C Result operation. Scalar literals use prepared native bits
through the registered `channel.scalar_literal_<profile>` provider. The helper
has no separate operation key.

Inherit the complete [FMT-03 family contract](FMT-03_channel_editing_contract.md),
including its support matrix through FMT-02, resource/error rules and scalar-fill
lowering. The public authoring helper `swizzle_channels` receives a graph,
`base` edge, any connected scalar edges, static slot expressions and the family
parameters. It returns one `values` edge handle. It is not a new native registry
key or an alias of the old `channel.swizzle` operation.

## Interface and meaning

Resolve base channel axis a through an explicit input description or `axis`. The static
ordered `slots` list contains at least one entry; each entry is a unique exact
base index/name/role selection or a reference to a same-dtype shape-[1] scalar.
The word unique applies to name/role resolution, not to repeated selections:
several output slots may read the same base channel or scalar. A typed literal
is an authoring shortcut for an equivalent scalar source. Raw uses explicit
axis/index selections; literals and connected scalars preserve all sample bits.

For base cell rank r and shape S with its channel axis removed, output cell rank
remains r and shape is insert(S,a,len(slots)). The full sample rank, including
batch and cell axes, is at most 8. No spatial dimension or channel-axis position
changes. Do not implicitly keep alpha: it must appear in slots when desired.
No zero-channel result, implicit constant, squeeze or broadcast is introduced.

Each output coordinate j selects slots[j[a]]. A base index s reads
base[insert(erase(j,a),a,s)]; a scalar reads V[0]. These are exact byte copies,
including HDR/negative values, signed zeros, infinities and NaN payloads. No
color conversion, alpha reassociation or numerical interpretation is applied.

## Explicit descriptions and graph expansion

Output samples carry no description. The caller supplies the source description
for named selection and separately configures any downstream color consumer.
A scalar in slot 3 does not acquire an alpha label.

Lower to one FMT-02C mapping row per output slot. Connected scalars use explicit
scalar source records; typed literals add scalar-literal nodes. Retain the base
Descriptor dependency even for all-constant output and use transactional graph
expansion, the selected CPU profile and the family layout policy.

## Fixed execution and ownership

The helper expands to fixed FMT-02C calls and, for typed literals, fixed scalar providers. Each retained output tile computes all of its effective source expressions. Lowering deduplicates the mapped source-grid tile union per port; unused ports retain schema dependencies. A constant or replacement-only request avoids base payload only when every destination in the normalized output tiles avoids base samples. Spatial tiles may contain multiple channels.

Auto/materialize copy to independently owned dense output tiles with zero scratch. Nonempty view fails with schema-only dependencies; Empty validates static inputs and emits no task. All copied bits are preserved, cancellation is checked between at most 256 samples, and a failed call exports no partial result. The family defines compatible rebinding, concurrent Runs and output lifetime.

## Independent acceptance fixtures

Use Float32 base [1,2,4] with straight RGB and independent alpha descriptions:

```
[[[10,20,30,0.4], [11,21,31,0.6]]]
```

Slots [2,1,0,3] give `[30,20,10,0.4]` and `[31,21,11,0.6]` at the two pixels.
A downstream description can name those slots B/G/R/alpha, with R=2, G=1 and B=0; this configuration is not an output property. At output (0,1,0), the copied value is base (0,1,2)=31. Other channels have no effect on that mathematical output value. An explicit canonical R/G/B target
description supplied to a later consumer changes its interpretation but leaves those copied bytes intact.

Slots [0,0,constant(V)] with Float32 V=[0.5] produce [10,10,0.5] and
[11,11,0.5]. Duplicate R selections do not implicitly form an RGB group. If the normalized output tiles contain only scalar-backed slots, they read V[0] without base pixels. Changing V to -0.0 or
a Float32 NaN with payload 0x7fc01234 changes only that slot's output bits, with
no shape or mapping change. Use byte comparison, not floating equality.

Identity slots [0,1,2,3] preserve samples; explicit
materialize still requests independent backing. Also cover one slot, all
constant slots, rank-one and rank-eight bases, arbitrary channel axes, name/role
ambiguity, literal/scalar dtype mismatch, and invalid/empty selections.

The independent oracle enumerates output coordinates and copies bytes from an
independently evaluated source expression; separately derive complete output and source tile closure. Validate lowered graphs against these results. The conceptual public
workflow is `base + scalar -> swizzle_channels expansion -> named values`.
Implementation must provide actual compile/execute commands, a partial channel
request and checked output.
