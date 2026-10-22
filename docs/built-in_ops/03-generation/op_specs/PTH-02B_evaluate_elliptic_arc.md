---
spec_schema_version: 1
id: PTH-02B
parent_id: PTH-02
function: evaluate_elliptic_arc
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.evaluate_elliptic_arc_v1_strict
  - path.evaluate_elliptic_arc_v1_accelerated_apple_silicon
  - path.evaluate_elliptic_arc_v1_accelerated_x86_64
---

# PTH-02B: evaluate elliptic arc

Inherit [PTH-02](PTH-02_evaluation_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Arc payload(c,u,v,theta0,sweep or FullTurn direction),segment_indices[N],t[N]; four
evaluation outputs as PTH-02A.

## Parameters and domain

Schema-valid arc, radians, t in [0,1], dtype; FullTurn direction +/-1.

## Mathematical specialization

theta=theta0+t*sweep; P=c+u*cos(theta)+v*sin(theta);
derivative=sweep*(-u*sin(theta)+v*cos(theta)). FullTurn uses exact direction*2*pi; at
t=1 reuse t=0 position and derivative bits. No rounded2pi closure test.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional O(N), certified trig/range reduction; nonorthogonal nondegenerate axes
permitted.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Circle quadrants, ellipse nonconstant speed, FullTurn endpoint bit identity and huge
angles.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
