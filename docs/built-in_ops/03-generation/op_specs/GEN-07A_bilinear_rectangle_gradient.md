---
spec_schema_version: 1
id: GEN-07A
parent_id: GEN-07
function: bilinear_rectangle_gradient
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# GEN-07A: bilinear rectangle gradient

Inherit [GEN-07](GEN-07_mesh_gradients_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Positive-area bounds[4] and controls[2,2,C], C>=1; values[H,W,C].

## Parameters and domain

canvas/dtype; outside=pad default or reject; finite numeric components. In reject mode, every consumed query in the complete selected output tiles lies inside the parameter domain as an unchecked premise.

## Mathematical specialization

u=(px-xmin)/(xmax-xmin), v similarly. Pad clamps u/v. Return
RN_T((1-u)(1-v)C00+u(1-v)C10+(1-u)vC01+uvC11) per component, not two rounded mixes. No
color or alpha semantics.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional pixels; all four control vectors are assumed valid.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Corner hits, four-corner mean, non-square bounds, four-component transparent-looking
data still interpolated numerically.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
