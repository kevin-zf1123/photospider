---
spec_schema_version: 1
id: GEN-04F
parent_id: GEN-04
function: siemens_star
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.siemens_star_v1_strict
  - generation.siemens_star_v1_accelerated_apple_silicon
  - generation.siemens_star_v1_accelerated_x86_64
---

# GEN-04F: siemens star

Inherit [GEN-04](GEN-04_test_patterns_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

center[2], phase[1]; values[H,W].

## Parameters and domain

canvas/dtype; cycles n=1..4096, phase radians; lo/hi default 0/1.

## Mathematical specialization

Away from center use RN_T(lo+(hi-lo)*(1+cos(n*atan2(dy,dx)+phase))/2). Center is
RN_T((lo+hi)/2). This is a cosine star, not thresholded spokes.

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

n=4, phase=0 gives hi along axes, midpoint at center; no artificial black seam.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
