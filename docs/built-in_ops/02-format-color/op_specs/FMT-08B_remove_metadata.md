---
spec_schema_version: 1
id: FMT-08B
parent_id: FMT-08
function: remove_metadata
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu
verification_status: focused_and_installed_result_tests_passed
---

# FMT-08B: remove selected semantic descriptions or annotations

Inherit the complete [FMT-08 contract](FMT-08_metadata_assignment_contract.md).
The `remove_metadata` authoring helper takes a workflow document, input Result
edge, static targets and family options, returning one `values` edge. It lowers
to [A assign metadata](FMT-08A_assign_metadata.md) in patch mode with no set
entries and the target list as remove. It creates no separate registry key; the
lowered node selects A's CPU profile. Result operation ABI is 2,
WorkflowDocument is 4 and OperationTraits is 21. FMT-08B remains Proposed.
Focused Result tests pass for metadata edits, Result execution/global behavior,
resource budgets and image contracts; the public workflow and two installed
consumer tests pass. The metadata-to-`channel.extract` configuration/resource
chain is covered; `channel.assemble` composition is outside this evidence.

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

B inherits A's same-coordinate sample support and dirty mapping, with Result Need
role mask 9 (Data | Descriptor) and no Validation or Control role. View can
retain backing without loading samples; materialize copies only requested bits.
Sample values are not scanned. Upstream failures retain their original scope.
Static preparation validates the retained metadata target and source schema.

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

The public [metadata workflow](../../../../examples/metadata_workflow/README.md)
executes assign and remove Result nodes and checks exact sample bits. The older
[metadata performance workload](../../../../examples/metadata_performance/README.md)
covers the previous Value/planar path and is not current Result performance
evidence.
