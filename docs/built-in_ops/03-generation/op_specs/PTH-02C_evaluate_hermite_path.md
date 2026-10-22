---
spec_schema_version: 1
id: PTH-02C
parent_id: PTH-02
function: evaluate_hermite_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.evaluate_hermite_path_v1_strict
  - path.evaluate_hermite_path_v1_accelerated_apple_silicon
  - path.evaluate_hermite_path_v1_accelerated_x86_64
---

# PTH-02C: evaluate hermite path

Inherit [PTH-02](PTH-02_evaluation_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

controls[S,4,2]=(P0,P1,d0,d1),segment_indices[N],t[N]; four evaluation outputs.

## Parameters and domain

dtype; d0/d1 are derivatives with respect to normalized t, not unit tangents or relative
handles.

## Mathematical specialization

P=(2t^3-3t^2+1)P0+(t^3-2t^2+t)d0+(-2t^3+3t^2)P1+(t^3-t^2)d1. Differentiate exactly.
Bezier conversion uses exact P0+d0/3 and P1-d1/3 intermediates unless separately
published.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional O(N) exact polynomial reference.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Endpoint derivative hits, straight-line derivatives, exact Hermite/Bezier algebra
agreement, zero tangent validity.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
