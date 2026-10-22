---
spec_schema_version: 1
id: PTH-02E
parent_id: PTH-02
function: evaluate_pathset
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# PTH-02E: evaluate pathset

Inherit [PTH-02](PTH-02_evaluation_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Runtime implementation

The public `ps::generation::evaluate_pathset` helper in
`photospider/ops/generation.hpp` implements this composition.

## Interface and representation

PathSet,segment_indices[N],t[N]; four evaluation outputs.

## Parameters and domain

Supported authorities/degrees inherit PTH-02A-D; Core IDs are verb rows (M invalid,Z
closing edge), primitive IDs are reference rows.

## Mathematical specialization

Dispatch by the checked static schema, preserving original segment IDs and explicit rounding.
Z runs from current point to M. A Primitive Close reference evaluates the line between its specified mathematical endpoints: P(t)=(1-t)*end+t*start, derivative=start-end. Evaluate those endpoints under their primitive mathematics and apply only the declared final rounding; no intermediate Float64 endpoint is invented. No implicit global-parameter segment selection.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional queries with valid path associations assumed; requested position alone
need not compute tangent.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Mixed L/Q/C/Z, queries selecting evaluable verb rows, separate/joint outputs, empty path with query
NoSolution.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
