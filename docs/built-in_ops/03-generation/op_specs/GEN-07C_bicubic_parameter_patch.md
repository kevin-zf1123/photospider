---
spec_schema_version: 1
id: GEN-07C
parent_id: GEN-07
function: bicubic_parameter_patch
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# GEN-07C: bicubic parameter patch

Inherit [GEN-07](GEN-07_mesh_gradients_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

bounds[4], controls[4,4,C], C>=1; values[H,W,C].

## Parameters and domain

canvas/dtype; outside=pad/reject; reject mode requires in-domain queries throughout the complete selected output tiles as an unchecked premise. One regular parameter patch, no curved-geometry
inverse mapping.

## Mathematical specialization

Use GEN-07A u/v and RN_T(sum_i,j B_i^3(u)*B_j^3(v)*controls[j,i]) per component. All 16
terms form one expression. Shared four boundary controls give C0 continuity across
explicitly assembled patches; no implicit stitching.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

O(16*C*area(Q)); finite controls are an unchecked premise.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Constant control vectors, cubic edges, corner hits; interior controls need not be
interpolated through. Fourth component is ordinary numeric data.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
