---
spec_schema_version: 1
id: GEN-05C
parent_id: GEN-05
function: angular_gradient_coordinate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.angular_gradient_coordinate_v1_strict
  - generation.angular_gradient_coordinate_v1_accelerated_apple_silicon
  - generation.angular_gradient_coordinate_v1_accelerated_x86_64
---

# GEN-05C: angular gradient coordinate

Inherit [GEN-05](GEN-05_gradient_coordinates_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

center[2], offset_turns[1]; t[H,W].

## Parameters and domain

canvas/dtype; finite offset in turns; center_policy=zero.

## Mathematical specialization

At center output +0. Otherwise q=atan2(dy,dx)/(2*pi)-offset_turns, then
RN_T(q-floor(q)). Exact seam maps to +0; an interior value rounding to 1 remains 1. No
nextDown or second wrap.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

O(area(Q)); certified branch and period classification.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Cardinal directions, offset seam, signed axes and values rounding to the upper endpoint.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
