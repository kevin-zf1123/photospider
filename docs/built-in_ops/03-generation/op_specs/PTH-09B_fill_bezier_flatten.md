---
spec_schema_version: 1
id: PTH-09B
parent_id: PTH-09
function: fill_bezier_flatten
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# PTH-09B: fill bezier flatten

Inherit [PTH-09](PTH-09_fill_contract.md), [GEN common](GEN_common_contract.md), and the
applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Core/Bezier PathSet; coverage[H,W] and optional GeometryReport.

## Parameters and domain

canvas/dtype/fill_rule; epsilon_geom_px default0.05 and flatten budgets; topology
allow_change default or reject_unproven.

## Mathematical specialization

Publish PTH-04D's deterministic polyline, then apply PTH-09A. Geometry approximation and
final area RN are distinct stages. Strict topology needs proof; default reports
unverified unless a change is established. Do not claim source-curve area
error<=epsilon_geom_px.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional coverage with accounted whole polyline controls/intermediate certificate.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Line equivalence, epsilon-dependent geometry, near-contact topology and actual ROI
computation.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
