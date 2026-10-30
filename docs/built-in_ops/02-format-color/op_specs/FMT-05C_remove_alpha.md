---
spec_schema_version: 1
id: FMT-05C
parent_id: FMT-05
function: remove_alpha
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# FMT-05C: remove one group's alpha relationship

The public `format::remove_alpha` helper appends a Result composition using registered mapped assembly for channel-bearing inputs, or `core.identity` for component Gray identity. These paths add no alpha operation key. The helper takes one single-tensor Result and publishes one such Result. It supports all seven copy dtypes, preserves the batch prefix and exact sample bits, and performs no sample-domain validation.

The cell-axis `axis` excludes the Result batch prefix. Full sample rank, including batch and cell axes, is at most 8; full sample count is at most 2^40.

## Interface

Inherits group, required input_description, cell-axis `axis`, layout and profile from the [FMT-05 contract](FMT-05_alpha_editing_contract.md). Static missing_alpha defaults to error; explicit identity permits unchanged samples when the selected group has no alpha relationship. An absent group or malformed description is still an error. There is no restore_straight/keep_samples parameter: input is straight and color samples are always preserved bit-for-bit.

If the explicit input description gives the group alpha, the helper inspects whether another group references that slot. The sample channel is deleted only when it is private; otherwise the shared channel is retained. The helper attaches no revised description to the result; the caller supplies any removed-alpha interpretation to later consumers. An existing channel axis is kept even when its new extent is one. No squeeze, background composite or color rescaling occurs.

## Mathematics

Let alpha occupy original slot j. If unreferenced after detaching the group, L=[0,...,j-1,j+1,...,C-1]. Output slot k copies original slot L[k] at the same nonchannel coordinate. For shared alpha, L is the identity list. Channel-bearing input lowers to mapped assembly and returns `values`; its `expected_inputs` assertion covers the canonical structural schema and spatial topology of the source.

An axis-free Gray input with `missing_alpha=identity` lowers to `core.identity` and returns `value`. This branch adds no `expected_inputs` or `expected_source` assertion and does not apply the helper layout policy.

Finite hidden color at former alpha=0 is preserved.

## Behavior

Requested output samples read only their mapped original input samples. Removed-alpha samples are not read or validated. For shared alpha, that channel's own output still depends on its original samples. Auto/materialize copy into independent dense output, including missing_alpha=identity; a nonempty view fails `InvalidArgument / ViewUnavailable`; Empty performs static checks only.

## Acceptance

Straight RGBA [0.5,-2,4,0] becomes RGB [0.5,-2,4] with identical color bits. Straight Gray+alpha shape [H,W,2] becomes [H,W,1], retaining its channel axis. Removing alpha never uses the reciprocal of zero or clears hidden color. For [R,A,G,B], private A removal yields [R,G,B], and group indices remap from [0,2,3] to [0,1,2]. If A is shared with another group, all channels remain.

Missing alpha fails by default. Explicit identity preserves samples. Integer alpha removal preserves every surviving integer bit. Invalid/NaN discarded alpha and unrequested invalid color peers add no sample failure. Cover arbitrary alpha positions, shared references, cell axes, batch prefixes and rollback of failed helper expansion.
