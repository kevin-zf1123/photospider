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

Inherits the [FMT-03 family contract](FMT-03_channel_editing_contract.md). The public authoring helper `swizzle_channels` receives a graph, `base` edge, any connected scalar edges, static slot expressions and the family parameters, and returns one `values` edge handle. The helper has no independent registry key and is not an alias of `channel.swizzle`.

## Interface

The base channel axis a is resolved through an explicit input description or `axis`. The static ordered `slots` list contains at least one entry; each entry is an exact base index/name/role selection or a reference to a same-dtype shape-[1] scalar. Name/role selections resolve to a unique channel, but several output slots may read the same base channel or scalar. A typed literal is an authoring shortcut for an equivalent scalar source. Raw uses explicit axis/index selections; literals and connected scalars preserve all sample bits.

For base cell rank r and shape S with its channel axis removed, output cell rank remains r and shape is insert(S,a,len(slots)). The full sample rank, including batch and cell axes, is at most 8. No spatial dimension or channel-axis position changes. Alpha is not implicitly kept: it must appear in slots when desired. No zero-channel result, implicit constant, squeeze or broadcast is introduced.

## Mathematics

Each output coordinate j selects slots[j[a]]. A base index s reads base[insert(erase(j,a),a,s)]; a scalar reads V[0]. These are exact byte copies, including HDR/negative values, signed zeros, infinities and NaN payloads. No color conversion, alpha reassociation or numerical interpretation is applied.

## Behavior

Output samples carry no description. The caller supplies the source description for named selection and separately configures any downstream color consumer. A scalar in slot 3 does not acquire an alpha label.

The base descriptor remains a static dependency even for all-constant output. Connected scalars and typed literals follow the family's exact-bit source rules. A constant-only request needs no base payload when every destination of the requested output avoids base samples. Auto/materialize produce independent dense output; a nonempty view fails; Empty emits no payload.

## Acceptance

Float32 base [1,2,4] with straight RGB and independent alpha descriptions:

```
[[[10,20,30,0.4], [11,21,31,0.6]]]
```

Slots [2,1,0,3] give `[30,20,10,0.4]` and `[31,21,11,0.6]` at the two pixels. A downstream description can name those slots B/G/R/alpha; this configuration is not an output property. At output (0,1,0), the copied value is base (0,1,2)=31. An explicit canonical R/G/B target description supplied to a later consumer changes its interpretation but leaves those copied bytes intact.

Slots [0,0,constant(V)] with Float32 V=[0.5] produce [10,10,0.5] and [11,11,0.5]. Duplicate R selections do not form an RGB group. Changing V to -0.0 or a Float32 NaN with payload 0x7fc01234 changes only that slot's output bits, with no shape or mapping change. Comparison is bytewise, not floating equality.

Identity slots [0,1,2,3] preserve samples; explicit materialize still requests independent backing. Also cover one slot, all constant slots, rank-one and rank-eight bases, arbitrary channel axes, name/role ambiguity, literal/scalar dtype mismatch, and invalid/empty selections.

## Conceptual DAG

`base + scalar -> swizzle_channels expansion -> named values`
