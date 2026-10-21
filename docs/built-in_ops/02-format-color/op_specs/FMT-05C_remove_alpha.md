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
and installed in `photospider/ops/format/alpha.hpp`. It appends an executable Result
composition using registered mapped assembly for channel-bearing inputs, or
`core.identity` for component Gray identity. These paths add no
alpha operation key. The helper takes one single-tensor Result
and publishes one such Result. It supports all seven copy dtypes, preserves the
batch prefix and exact sample bits, and performs no sample-domain validation.

The cell-axis `axis` excludes the Result batch prefix. Full sample rank,
including batch and cell axes, is at most 8; full sample count is at most 2^40.
The helper is a compile-time composition over registered Result operations; it
does not add an alpha-specific runtime key.

## Interface and output inference

Inherit group, required input_description, cell-axis `axis`,
layout and profile. `axis` excludes the batch prefix. Static missing_alpha defaults to error; explicit identity permits an
unchanged samples when the selected group has no alpha relationship.
An absent group or malformed description is still an error, not an identity case.
There is no restore_straight/keep_samples parameter: canonical input is straight
and color samples are always preserved bit-for-bit.

If the explicit input description gives the group alpha, inspect whether another
group references that slot. Delete the sample channel only when it is private;
otherwise retain the shared channel. The helper does not mutate the source
configuration or attach a revised one to the result. The caller supplies any
removed-alpha interpretation to later consumers. Keep an existing channel axis even when its new
extent is one. No implicit squeeze, background composite or color rescaling
occurs. All family copy dtypes are supported without decoding;
no finite/range sample validation is introduced by removal.

## Lowering, exact map and metadata

Let alpha occupy original slot j. If unreferenced after detaching the group,
construct L=[0,...,j-1,j+1,...,C-1]. Output slot k copies original slot L[k]
at the same nonchannel coordinate. For shared alpha, L is the identity list.
Channel-bearing input lowers to mapped assembly and returns `values`. Its
`expected_inputs` assertion covers the canonical structural schema and spatial
topology of the source. No output description is produced.

An axis-free Gray input with `missing_alpha=identity` lowers to `core.identity`
and returns `value`. This branch adds no `expected_inputs` or `expected_source`
assertion and does not apply the helper layout policy. core.identity emits explicit COPY tasks, preserving samples in independently owned output tiles.

Finite hidden color at former alpha=0 is preserved. Callers configure downstream
groups and alpha references explicitly. Mapped removal copies surviving channels into independently owned dense output tiles.

Requested output samples read only their mapped original input samples. No
removed-alpha samples are numerically read or validated. Complete output tiles determine all surviving source coordinates, and source-grid closure may retain tiles containing discarded channels. Incremental dirty propagation is outside this execution contract.
For shared alpha, that channel's own output still depends on its original samples.
Static group/reference changes reinfer the mapping. A required upstream producer
can still fail at its own complete-input scope; C does not cancel such work.

Channel-bearing mapped assembly uses fixed ABI 8. Auto/materialize copy selected output tiles with zero scratch, including missing_alpha=identity. Every nonempty view task has only schema dependencies and fails InvalidArgument/ViewUnavailable; Empty performs static checks with no task. Axis-free identity does not apply the helper layout policy. The family defines transactional authoring, cancellation, ordered call retirement and independent output ownership.

## Independent acceptance

Straight RGBA [0.5,-2,4,0] becomes RGB [0.5,-2,4] with identical color bits.
Straight Gray+alpha shape [H,W,2] becomes [H,W,1], retaining its channel axis.
Removing alpha never uses the reciprocal of zero or clears hidden color.
For [R,A,G,B], private A removal yields [R,G,B], and group indices remap from
[0,2,3] to [0,1,2]. If A is shared with another group, all channels remain and
the caller supplies the selected group's new interpretation downstream.

Missing alpha fails by default. Explicit identity preserves samples; layout applies only to the mapped branch. Integer alpha removal preserves every surviving
integer bit without numeric normalization. Invalid/NaN discarded alpha and
unrequested invalid color peers add no sample failure.

Use an independent retained-slot oracle and exact bytes for arbitrary alpha
positions, shared references, cell axes, batch prefixes, disjoint regions and
tile boundaries. Check surviving output owners after context retirement,
final release, low budgets, cancellation and rollback of failed helper expansion.
Create a temporary public compile/execute check for the helper expansion and
remove it after the operation change is validated.
