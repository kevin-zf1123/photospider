---
spec_schema_version: 1
id: GEN-05D
parent_id: GEN-05
function: diamond_gradient_coordinate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.diamond_gradient_coordinate_v1_strict
  - generation.diamond_gradient_coordinate_v1_accelerated_apple_silicon
  - generation.diamond_gradient_coordinate_v1_accelerated_x86_64
---

# GEN-05D: diamond gradient coordinate

Inherit [GEN-05](GEN-05_gradient_coordinates_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

center[2], positive radii[2]; t[H,W].

## Parameters and domain

canvas/dtype.

## Mathematical specialization

RN_T(abs(px-cx)/rx+abs(py-cy)/ry); radii are axis intercepts, not Euclidean distances.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

O(area(Q)).

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

p-c=(1,1), radii=(2,2) gives 1; center zero.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
