---
spec_schema_version: 1
id: GEN-03C
parent_id: GEN-03
function: polygon_coverage
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# GEN-03C: polygon coverage

Inherit [GEN-03](GEN-03_basic_shapes_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Runtime implementation

The public `ps::generation::polygon_coverage` helper in
`photospider/ops/generation.hpp` implements this composition.

## Interface and representation

CoreVerbs M/L/Z polygon PathSet; coverage[H,W].

## Parameters and domain

canvas, dtype default Float64, fill_rule=nonzero/evenodd.

## Mathematical specialization

Compose PTH-09A exact polygon-area filling; self-intersections and holes use the
selected fill rule. Open contours close only for this fill consumption.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional output with complete path mathematical support.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Repeated same-oriented contours differ under nonzero/evenodd; bow-tie, shared edges and
orientation reversal.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
