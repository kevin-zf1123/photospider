---
spec_schema_version: 1
id: GEN-02A
parent_id: GEN-02
function: coordinates_xy
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.coordinates_xy_v1_strict
  - generation.coordinates_xy_v1_accelerated_apple_silicon
  - generation.coordinates_xy_v1_accelerated_x86_64
---

# GEN-02A: coordinates xy

Inherit [GEN-02](GEN-02_coordinate_grid_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

No Tensor inputs; coordinates[H,W,2] Float32/64, component order xy.

## Parameters and domain

canvas, dtype default Float64; space=pixel/normalized_edge, default pixel.

## Mathematical specialization

pixel returns RN_T(origin_x+x+1/2), RN_T(origin_y+y+1/2). normalized_edge returns
RN_T((x+1/2)/W), RN_T((y+1/2)/H). Construct exact rational coordinates before rounding.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

O(area(Q)); no upstream samples.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

W=4 gives x=(1/8,3/8,5/8,7/8); 1x1 normalized center=(0.5,0.5); origin=(10,-2) gives
first center=(10.5,-1.5).

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
