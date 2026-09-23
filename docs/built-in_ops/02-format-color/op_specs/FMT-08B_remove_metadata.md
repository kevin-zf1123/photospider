---
spec_schema_version: 1
id: FMT-08B
parent_id: FMT-08
function: remove_metadata
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-08B: remove selected semantic descriptions or annotations

Inherit the complete [FMT-08 contract](FMT-08_metadata_assignment_contract.md).
The proposed authoring helper `remove_metadata` takes a graph, input edge,
static targets and family parameters, returning one `values` edge. It lowers
to [A assign metadata](FMT-08A_assign_metadata.md) in patch mode with no set
entries and the target list as remove. There is no new native registry key.

## Interface and lowering

Targets identify exact semantic fields/subtrees, groups/channel descriptions or
explicit opaque annotation keys. The registered semantic root can be selected
to clear known semantic descriptions; annotations require their own explicit
targets. No wildcard or guess from channel count is allowed. Resolve all index/
name/role selectors against the original input using exact unique matches.
Duplicate or overlapping targets fail, including an ancestor and its descendant.

Parameters are targets, dependencies=error|cascade, missing=error|ignore,
layout=auto|view|materialize and profile (strict by default or named CPU profile).
Defaults and validation are A's. Empty targets is a valid semantic identity.
A missing target fails by default; ignore permits cleanup of optionally present
fields, but never excuses an ambiguous selector or invalid schema. All structure,
sample bytes and channel positions remain unchanged.

Build the equivalent A node and output handle transactionally using existing
collision-aware authoring IDs. Validate parameters and stage the expansion before
appending; failure leaves the caller graph unchanged. Do not perform source
sample reads or synthesize a changed image while constructing the helper.
Normal host graph/resource limits and selected-profile backend constraints apply.

## Dependency cleanup and output meaning

A valid independent deletion changes only that field/annotation. A deletion that
breaks required references fails under error. Cascade removes only the schema-
declared affected dependency closure until the retained target is valid. It may
remove an optional alpha reference without deleting its color group; if required
color-space information disappears, it may remove the complete dependent group
while retaining independent component descriptions. It never chooses another
alpha, profile, model, unit or grid to replace the deleted one.

Removing an alpha relationship or channel description leaves the alpha plane's
bytes in the tensor. FMT-05C separately removes an unreferenced alpha data channel
and changes logical shape; B does neither. Removing an encoding declaration must
not leave a contradictory complete native-color claim. Removing metadata creates
no sample-validity certificate or permission to use missing pixels.

Opaque annotations survive unrelated deletions, including deletion of the
registered semantic root. Only explicit annotation targets remove them. Resource
references removed from the result release its semantic ownership when possible;
source/view/other-consumer ownership may keep backing or profiles alive. No
immediate deallocation of shared storage is promised.

## Demand, layout and errors

B inherits A's exact q->q sample support and dirty mapping, Descriptor dependencies
and absence of pixel Validation/Control support. View can retain backing without
loading samples; materialize copies only requested bits. Unrequested values,
including NaN/invalid alpha, are not scanned. Upstream required failures retain
their original scope. Static inference still validates the retained target.

Missing=ignore and empty targets do not override a forced materialize request.
All outputs have independent immutable metadata, and all modes preserve actual
storage constraints. Use A's error phases, resource/cancellation limits and cache/
optimizer requirements. A downstream consumer demanding removed metadata must
receive an explicit missing-description error or a new assign, not recover a
hidden old descriptor from the source.

## Independent acceptance fixtures

For straight RGBA shape [H,W,4], removing only the selected group's alpha relation
leaves shape [H,W,4] and every byte unchanged. The group is now straight without
alpha; channel 3 still exists. Contrast with an explicit later channel-removal
operation, which changes shape. Shared other-group references survive where valid.

Deleting a missing annotation fails under error and becomes a no-op under ignore.
An empty target list is identity. Clearing the semantic root preserves an opaque
application note; naming that note as a separate target removes it. Ambiguous
role selectors fail even with missing=ignore.

A group referencing a removed required space/profile fails under error. With
cascade, remove only that group's dependent description and retain unrelated
groups and component labels. Verify input metadata and another consumer remain
unchanged. Valid pixel bytes are still available as generic/component data; no
RGB/Gray model is inferred merely because three channels remain.

Compare helper inference/results against an independently constructed expected
metadata tree and byte-identical input at all requested coordinates. Check all
layouts, partial/tile-crossing requests, resource lifetimes after context release,
missing coverage, graph rollback, low budgets and cancellation. Conceptual graph:
input -> B -> generic/component processing -> explicit A when a new interpretation
is needed. Runnable public workflow evidence belongs to implementation; none is
claimed by this Proposed specification.
