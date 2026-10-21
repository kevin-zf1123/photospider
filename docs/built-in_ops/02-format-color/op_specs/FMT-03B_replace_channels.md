---
spec_schema_version: 1
id: FMT-03B
parent_id: FMT-03
function: replace_channels
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu
clarification_status: complete
---

# FMT-03B: replace selected channel slots

Runtime contract: this installed helper expands transactionally into the
registered FMT-02C Result operation. Scalar literals use prepared native bits
through the registered `channel.scalar_literal_<profile>` provider. The helper
has no separate operation key.

Inherit the complete [FMT-03 family contract](FMT-03_channel_editing_contract.md).
The public authoring helper `replace_channels` takes a graph, `base`, any
external spatial/scalar source edges, a static replacement list and the family
parameters. It returns one immutable `values` edge. It expands into FMT-02C
and any conforming scalar sources; there is no independent native key.

## Interface and simultaneous replacement

Each replacement names exactly one destination in the original base by index,
name or role and supplies one source expression. Name/role namespaces are
separate, exact and uniquely resolved against the original explicit input descriptions; raw uses
indices. Reject duplicate destinations even if both rows would write equal
bytes. An empty replacement list is a valid identity. No output slot is added,
removed or moved: output shape, dtype and channel-axis position equal the base.

Source expressions may select an original base channel, an external component
tensor, a selected external channel or an explicit scalar constant. External
spatial sources match the base nonchannel shape under
the family rules; their own channel axes may differ. Scalars are same-dtype
shape-[1] sources, with typed literal shortcuts. A/B's positive cell-rank, checked extent and no-implicit-broadcast rules
apply; batch plus cell rank is at most 8.

For output slot k, use its replacement expression if listed and original base
channel k otherwise. Every expression observes the original immutable inputs.
For example, replacing R from G and G from R is a simultaneous swap, regardless
of replacement-list order. There is no write-after-write interpretation and no
mutation visible to another consumer of base.

## Output interpretation and expansion

Replacement copies source bits to destination indices. Unlisted destinations
retain their sample bits. No source or destination description is attached to
the result. A caller can supply an RGB/alpha description to a later consumer,
independently of whether replacement values came from Gray, color or scalar data.

Replacing alpha preserves other samples exactly and performs no association or
coverage scan. FMT-04/05 provide the separate alpha-specific operations.
Compile identity mappings for all base slots, substitute the resolved sources
at replaced destinations, and preserve Descriptor dependencies for all connected
inputs, including unused sources. Scalar source records and typed literals
follow the family lowering; expansion is transactional.

## Fixed execution and ownership

The helper expands to fixed FMT-02C calls and, for typed literals, fixed scalar providers. Each retained output tile computes all of its effective source expressions. Lowering deduplicates the mapped source-grid tile union per port; unused ports retain schema dependencies. A constant or replacement-only request avoids base payload only when every destination in the normalized output tiles avoids base samples. Spatial tiles may contain multiple channels.

Auto/materialize copy to independently owned dense output tiles with zero scratch. Nonempty view fails with schema-only dependencies; Empty validates static inputs and emits no task. All copied bits are preserved, cancellation is checked between at most 256 samples, and a failed call exports no partial result. The family defines compatible rebinding, concurrent Runs and output lifetime.

## Independent acceptance fixtures

Take Float32 base [1,2,4] with logical straight RGBA samples
`[[[10,20,30,0.4],[11,21,31,0.6]]]` and channel axis 2.
An external Gray component plane X has shape [1,2] and samples [[7,8]].
Replace destination R from X and alpha from same-dtype scalar V=[0.5].
Expected output is `[[[7,20,30,0.5],[8,21,31,0.5]]]`. A subsequent consumer may be explicitly configured as R/G/B/alpha. Base R and base alpha are not read for these replaced outputs.
Output (0,1,0) copies X[0,1]=8. A no-base/no-V dependency check must select complete output tiles containing only that replacement.

For the simultaneous swap R<-base G, G<-base R, the output is
`[[[20,10,30,0.4],[21,11,31,0.6]]]`; no R/G/B/alpha description is attached.
Reversing replacement-list order has the same result. A request only for output
R copies original G, independently of the original R value.

For mixed source axes, external X has shape [2,1,2], axis 0, with planes
[[100,101]] and [[200,201]]. Replacing B from X channel 1 produces B values
200 and 201 at the two pixels. An external [1,1] component does not silently
broadcast to the required [1,2] domain.

Verify empty-list identity and explicit materialization, repeated source use,
all-slots replacement, named target/source resolution, duplicate-target rejection,
wrong dtype/shape, rank-one scalar replacement, special-value bits and alpha
replacement without hidden color arithmetic. Test that every failed expansion
leaves the graph unchanged and that base values/metadata remain unchanged.

The oracle independently evaluates all source expressions against the original
inputs and derives source tile closure, including overwritten-value nonreads within each complete output tile.
The conceptual public workflow is `base + external plane + scalar ->
replace_channels expansion -> named values`. Implementation must supply actual
public commands and checked partial-region output.
