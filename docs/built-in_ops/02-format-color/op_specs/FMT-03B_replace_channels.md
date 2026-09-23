---
spec_schema_version: 1
id: FMT-03B
parent_id: FMT-03
function: replace_channels
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-03B: replace selected channel slots

Inherit the complete [FMT-03 family contract](FMT-03_channel_editing_contract.md).
The proposed authoring helper `replace_channels` takes a graph, `base`, any
external spatial/scalar source edges, a static replacement list and the family
parameters. It returns one immutable `values` edge. It expands into FMT-02C
and any conforming scalar sources; there is no independent native key.

## Interface and simultaneous replacement

Each replacement names exactly one destination in the original base by index,
name or role and supplies one source expression. Name/role namespaces are
separate, exact and uniquely resolved against original metadata; raw uses
indices. Reject duplicate destinations even if both rows would write equal
bytes. An empty replacement list is a valid identity. No output slot is added,
removed or moved: output shape, dtype and channel-axis position equal the base.

Source expressions may select an original base channel, an external component
tensor, a selected external channel or an explicit scalar constant. External
spatial sources match the base nonchannel shape and coordinate description under
the family rules; their own channel axes may differ. Scalars are same-dtype
shape-[1] sources, with typed literal shortcuts. A/B's positive-rank, checked
extent and no-implicit-broadcast rules apply.

For output slot k, use its replacement expression if listed and original base
channel k otherwise. Every expression observes the original immutable inputs.
For example, replacing R from G and G from R is a simultaneous swap, regardless
of replacement-list order. There is no write-after-write interpretation and no
mutation visible to another consumer of base.

## Destination metadata and expansion

By default, replaced positions retain their destination names, roles, units and
applicable group descriptions. The selected destination supplies explicit target
semantics for the replacement: copying Gray into R yields an R output component
without source override/raw. Unlisted component descriptions and values remain
unchanged. Replaced slots may receive explicit new target fields; structurally
incompatible groups must be removed or explicitly rebuilt coherently, as in the
family. No output sample-validity certificate is created by retaining a label.

Replacing alpha with a scalar preserves other color samples exactly. It does
not multiply/divide RGB or enforce a coverage interval. A later consumer that
uses coverage semantics checks its required values; canonical images remain
straight and carry no external alpha binding. Specialized
alpha editing/association is governed by FMT-04/05.

Compile C identity mappings for all base slots, substitute the resolved sources
at replaced destinations, and pass the computed destination metadata explicitly
to FMT-02C. This explicit output description is essential: C's normal default
source projection alone would give B the wrong semantics. Preserve Descriptor
dependencies for all connected inputs, including unused replacement sources,
and use the family's transactional graph expansion and selected CPU profile.

## Regional dependencies and resources

For each requested output sample, Data reads only its effective source expression.
Unchanged slots depend on their original base positions; replaced slots depend
on replacements. Overwritten base samples are not read unless selected as a
source for another requested destination. A base change propagates to every
effective expression that reads it, including swap destinations; it does not
automatically dirty the same-index overwritten output.

External source changes map only to slots selecting those components. A scalar
change affects all observed nonchannel positions of its selecting slots. Data
support deduplicates identical source coordinates; dirty support includes every
repeated use. There is no extra sample Validation or Control domain and no
sequential mutation dependency. Static descriptor checks remain required.

Use the family's scalar-fill/image-layout distinction, view proof, page budget,
owner retention, exact valid coverage and failure policy. An unchanged input
plane may be reused only when the resulting whole mapping remains legal; mixing
it with independent replacement owners does not create a new permitted image
storage form. Auto materializes when necessary. Forced view failure, metadata
errors and source/resource/cancellation outcomes retain the family categories.

## Independent acceptance fixtures

Take Float32 base [1,2,4] with logical straight RGBA samples
`[[[10,20,30,0.4],[11,21,31,0.6]]]` and channel axis 2.
An external Gray component plane X has shape [1,2] and samples [[7,8]].
Replace destination R from X and alpha from same-dtype scalar V=[0.5].
Expected output is `[[[7,20,30,0.5],[8,21,31,0.5]]]`; descriptions remain
R/G/B/alpha. Base R and base alpha are not read for these replaced outputs.
Requesting only (0,1,0) reads X[0,1]=8, no base pixels and no V.

For the simultaneous swap R<-base G, G<-base R, the output is
`[[[20,10,30,0.4],[21,11,31,0.6]]]`, still described as R/G/B/alpha by default.
Reversing replacement-list order has the same result. A request only for output
R depends on original G, so changing original R does not dirty that observation.

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
inputs and enumerates Data/dirty support, including overwritten-value nonreads.
The conceptual public workflow is `base + external plane + scalar ->
replace_channels expansion -> named values`. Implementation must supply actual
public commands and checked partial-region output. No runtime or benchmark is
claimed by this Proposed specification.
