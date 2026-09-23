---
spec_schema_version: 1
id: FMT-05C
parent_id: FMT-05
function: remove_alpha
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-05C: remove one group's alpha relationship

Inherit the complete [FMT-05 contract](FMT-05_alpha_editing_contract.md).
The proposed authoring helper `remove_alpha` takes a graph and described input
edge and returns one `values` edge. It expands into conforming channel mapping
and explicit metadata edits; no separate native registry key is introduced.

## Interface and output inference

Inherit group, metadata_mode, conditional metadata_override, axis, layout and
profile. Static missing_alpha defaults to error; explicit identity permits an
unchanged value/description when the selected group has no alpha relationship.
An absent group or malformed description is still an error, not an identity case.
There is no restore_straight/keep_samples parameter: canonical input is straight
and color samples are always preserved bit-for-bit.

If the group has alpha, remove only its internal alpha reference. When no other
declared reference uses that alpha slot, delete the channel, preserving every
other channel's order and remapping indices. Otherwise retain that shared slot
and other groups' references. Keep an existing channel axis even when its new
extent is one. No implicit squeeze, background composite or color rescaling
occurs. All family copy dtypes are supported with applicable encoding metadata;
no finite/range sample validation is introduced by removal.

## Lowering, exact map and metadata

Let alpha occupy original slot j. If unreferenced after detaching the group,
construct L=[0,...,j-1,j+1,...,C-1]. Output slot k copies original slot L[k]
at the same nonchannel coordinate. For shared alpha, L is the identity list.
Pass an explicitly revised target description to the FMT-02C/FMT-03 mapping so
that ordinary automatic metadata projection cannot restore the detached relation
or remove the still-valid straight color group. Metadata-only identity cases
still require coherent target metadata and the selected layout semantics.

The selected group remains a complete straight color group with no alpha.
Its finite hidden color at former alpha=0 is preserved. Remaining alpha groups
retain their own references; unrelated AOVs and component units are unchanged.
No old-alpha provenance creates an active external relation. Removing a logical
channel may leave backing retained by a view, under normal kernel ownership.

Requested output samples read only their mapped original input samples. No
removed-alpha samples or other color peers are read/validated. Dirty support
is the inverse retained-slot map; discarded samples have no output effect.
For shared alpha, that channel's own output still depends on its original samples.
Static group/reference changes reinfer the mapping. A required upstream producer
can still fail at its own Whole scope; C does not cancel such work.

Honor auto/view/materialize, including for missing_alpha=identity. Forced
materialize returns new owned requested coverage; forced view requires a legal
same-owner mapping. Use the family's transactional helper expansion, bounded
copy resources, cancellation, cache restrictions and error attribution.

## Independent acceptance

Straight RGBA [0.5,-2,4,0] becomes RGB [0.5,-2,4] with identical color bits.
Straight Gray+alpha shape [H,W,2] becomes [H,W,1], retaining its channel axis.
Removing alpha never uses the reciprocal of zero or clears hidden color.
For [R,A,G,B], private A removal yields [R,G,B], and group indices remap from
[0,2,3] to [0,1,2]. If A is shared with another group, all channels remain and
only the selected group's reference disappears.

Missing alpha fails by default. Explicit identity preserves samples and metadata
while honoring forced layout. Integer alpha removal preserves every surviving
integer bit without numeric normalization. Invalid/NaN discarded alpha and
unrequested invalid color peers add no sample failure.

Use an independent retained-slot oracle and exact bytes for arbitrary alpha
positions, shared references, ranks/axes, disjoint regions and tile boundaries.
Check surviving source/view owners after context retirement, final release,
low budgets, cancellation and rollback of failed helper expansion.
Conceptual workflow: straight image -> C -> straight image without selected
alpha. Public compile/execute examples remain an implementation requirement;
this specification claims no runtime implementation or performance result.
