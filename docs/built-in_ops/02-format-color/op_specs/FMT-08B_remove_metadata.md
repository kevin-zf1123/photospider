---
spec_schema_version: 1
id: FMT-08B
parent_id: FMT-08
function: edit_description
kind: sdk_function
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# FMT-08B: delete selected descriptions

Inherit the complete [FMT-08 contract](FMT-08_metadata_assignment_contract.md).
B is `format::edit_description` in patch mode with an empty `set` list and the
deletion paths in `remove`. It returns a new `TensorDescription`; it creates no
registry key, no authoring helper and no workflow node. FMT-08B remains Proposed.

## Interface

Targets identify exact description fields/subtrees, groups or channel
descriptions under `/semantic`. `/semantic` itself clears the description. No
wildcard or guess from channel count is allowed. Resolve all index/name/role
selectors against the original source using exact unique matches. Duplicate or
overlapping targets fail, including an ancestor and its descendant.

Options are `remove`, `dependencies=error|cascade` and `missing=error|ignore`.
An empty target list returns an equal description. A missing target fails by
default; `ignore` permits cleanup of optionally present fields, but never
excuses an ambiguous selector or invalid path. The tensor's structure, sample
bytes and channel positions are not involved.

## Dependency cleanup and meaning

A valid independent deletion changes only that field. A deletion that breaks
required references fails under `error`. Cascade removes only the affected
dependency closure until the retained description is valid. It may remove an
optional alpha reference without deleting its color group; if required
color-space information disappears, it may remove the complete dependent group
while retaining independent component descriptions. It never chooses another
alpha, profile, model, unit or grid to replace the deleted one.

Removing an alpha relationship or channel description from a description does
not remove the alpha plane from any tensor. FMT-05C separately removes an alpha
data channel and changes logical shape; B does neither. Removing an encoding
declaration must not leave a contradictory complete native-color claim. A
consumer that receives the shortened description and needs the removed field
fails with its ordinary missing-description error; it cannot recover the old
field from anywhere else.

## Errors

Use A's error rules: malformed or ambiguous paths, missing targets under `error`,
and failed protected cascade return `InvalidArgument`; a result incompatible with
the descriptor returns the `validate_tensor_description` status.

## Independent acceptance fixtures

For a straight RGBA description of shape [H,W,4], removing only the selected
group's alpha relation leaves a description whose group is straight without
alpha and whose channel table still has four entries. The tensor is untouched.

Deleting a missing channel field fails under `error` and is a no-op under
`ignore`. An empty target list returns an equal description. Ambiguous role
selectors fail even with `missing=ignore`.

A group referencing a removed required space/profile fails under `error`. With
`cascade`, remove only that group's dependent description and retain unrelated
groups and component labels. The source description is unchanged. No RGB/Gray
model is inferred merely because three channels remain.
