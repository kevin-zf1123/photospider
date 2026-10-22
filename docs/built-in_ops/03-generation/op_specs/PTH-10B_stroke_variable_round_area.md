---
spec_schema_version: 1
id: PTH-10B
parent_id: PTH-10
function: stroke_variable_round_area
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.stroke_variable_round_area_v1_strict
  - path.stroke_variable_round_area_v1_accelerated_apple_silicon
  - path.stroke_variable_round_area_v1_accelerated_x86_64
---

# PTH-10B: stroke variable round area

Inherit [PTH-10](PTH-10_stroke_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Polyline PathSet with finite nonnegative per-vertex widths from exactly one explicit source: a separate workflow-paired path_width Result or an independent width-evaluation input; coverage[H,W]. The PathSet carries no implicit width field.

## Parameters and domain

canvas/dtype; fixed round caps/joins; width linear in each edge's local t; binding association is an unchecked caller premise.

## Mathematical specialization

Region is union over edges and t in[0,1] of
disks(center=(1-t)P0+tP1,radius=((1-t)w0+t*w1)/2). Each edge equals convex hull of
endpoint disks; radius difference>=edge length yields the larger disk. Zero-length edge
uses larger radius; closed endpoint widths agree.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional pixels; algebraic tangents and exposed circular arcs, not sampled stamps or a
normal-offset trapezoid.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Constant width agrees with round/round A, zero-radius cone, contained-disk case,
repeated vertices and self-overlap union.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
