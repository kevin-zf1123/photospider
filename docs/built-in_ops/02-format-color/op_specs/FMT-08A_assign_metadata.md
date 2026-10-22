---
spec_schema_version: 1
id: FMT-08A
parent_id: FMT-08
function: edit_description
kind: sdk_function
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# FMT-08A: patch or replace an explicit description

Inherit the complete [FMT-08 contract](FMT-08_metadata_assignment_contract.md).
A is `format::edit_description` with `mode=patch` and `set` entries, or with
`mode=replace` and a complete `description`. It returns a new `TensorDescription`
for the caller to pass to a consuming operation. It is neither a validation
certificate, a numeric conversion, nor a workflow node; no registry key exists.
The specification status remains Proposed.

## Interface

Inputs are the source description, the target tensor's `TensorDescriptor`, and
`MetadataOptions`. Patch edits named fields with optional atomic deletions.
Replace substitutes a complete description and accepts no path edits.

Resolve selectors against the original source. A set of a whole group/subtree
replaces it completely; leaf updates retain unmentioned applicable siblings.
Reject duplicate/overlapping paths and order-dependent edit combinations. A
single call can change model/space and remove obsolete fields without producing
an invalid intermediate description. Unknown field names are errors.

The resulting description must be structurally self-consistent and fit
`descriptor`, even if the samples it will later describe violate its claimed
numerical meaning. A complete color group needs its required fields/references,
while independent component roles/units are allowed without a complete group. A
canonical image description remains straight with internal alpha. A description
of a premultiplied boundary payload must not claim that an alpha conversion
occurred.

## Reference transform

Let M be the source description and S the descriptor. For patch, construct
candidate N from M, apply the nonoverlapping set/remove operations atomically,
then resolve the selected dependency policy. For replace, N is the supplied
description. Check N against S. If successful, return N; M is unchanged.

`dependencies=error` rejects dangling/invalidated descriptions. Cascade may
remove affected dependent descriptions to a stable valid target, but cannot
delete newly assigned content or invent a replacement interpretation. An
unrepaired invalid explicit target fails. `missing=ignore` only ignores absent
remove targets. Empty patch returns an equal description; empty replacement
returns an empty description.

## Independent acceptance fixtures

- From a valid straight RGB description, patch its primaries to another complete
  valid primary definition without changing other applicable fields. Require a
  changed result, an unchanged source description, and no sample access.
- A component description assigning a coverage role is valid for a tensor whose
  samples contain 1.6; `edit_description` never reads samples. Supplying that
  sample to a consumer requiring coverage in [0,1] violates the caller premise.
  The consumer has no required sample-validation or diagnostic behavior.
- Atomically replace a complete RGB group with a complete Lab group description
  and remove obsolete dependent fields explicitly. An incomplete requested Lab
  group fails and cascade cannot erase it to manufacture success.
- Replace with an empty description returns an empty description; empty patch
  returns an equal description.
- Removing a required space description under error fails; under cascade the
  affected dependent group is removed and independent component names survive.
  New explicitly assigned fields that would be lost make it fail.
- A description whose channel table length or channel axis disagrees with
  `descriptor` fails; no description can change dtype or shape.
