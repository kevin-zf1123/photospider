---
spec_schema_version: 1
id: FMT-05C
parent_id: FMT-05
function: remove_alpha
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_subset
clarification_status: complete
---

# FMT-05C: remove one group's alpha relationship

FMT-05C remains Proposed. The public `format::remove_alpha` helper is compiled
and installed in `photospider/format/alpha.hpp`. It appends an executable Result
composition using registered mapped assembly for channel-bearing inputs, or
registered metadata assignment for component Gray identity. These paths add no
alpha operation key. The helper takes one single-tensor Result without fields
and publishes one such Result. It supports all seven copy dtypes, preserves the
batch prefix and exact sample bits, and performs no sample-domain validation.

The cell-axis `axis` excludes the Result batch prefix. Full sample rank,
including batch and cell axes, is at most 8; full sample count is at most 2^40.
The helper is a compile-time composition over registered Result operations; it
does not add an alpha-specific runtime key.

## Interface and output inference

Inherit group, metadata_mode, conditional metadata_override, cell-axis `axis`,
layout and profile. `axis` excludes the batch prefix. Static missing_alpha defaults to error; explicit identity permits an
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
Pass an explicitly revised target description to the FMT-02C mapping so that
ordinary source projection cannot restore the detached alpha reference or remove
the still-valid straight color group. Metadata-only component Gray identity
uses `metadata.assign` and retains same-coordinate Data support. Channel-bearing
identity still rebuilds its TensorDescription through mapped assembly; it keeps
the selected samples and group semantics while following assembly metadata
projection and layout rules. It does not promise byte-identical preservation of
the full source schema or opaque tensor facets. For component Gray identity, the
`expected_source` assertion uses `result-v1` with a digest of the complete canonical source schema and a separate physical
layout assertion. The canonical schema covers semantic metadata and spatial
axes; the layout assertion covers physical storage order and row pitch.
Channel-bearing mapped assembly uses its aggregate `expected_inputs` assertion,
which hashes the full canonical schema and layout framing for its inputs.
Metadata-only identity also honors the selected layout semantics.

The selected group remains a complete straight color group with no alpha; the
output description follows mapped-assembly projection rules.
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
positions, shared references, cell axes, batch prefixes, disjoint regions and
tile boundaries. Check surviving source/view owners after context retirement,
final release, low budgets, cancellation and rollback of failed helper expansion.
`test_alpha_authoring` exercises the public compile/execute path and passes.
Fourteen small alpha performance smoke cases pass their output oracle; no full
matrix is claimed.
