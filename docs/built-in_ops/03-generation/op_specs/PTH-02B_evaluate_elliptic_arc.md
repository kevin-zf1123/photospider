---
spec_schema_version: 1
id: PTH-02B
parent_id: PTH-02
function: evaluate_elliptic_arc
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.evaluate_elliptic_arc_v1_strict
  - path.evaluate_elliptic_arc_v1_accelerated_apple_silicon
  - path.evaluate_elliptic_arc_v1_accelerated_x86_64
---

# PTH-02B: evaluate elliptic arc

Inherit [PTH-02](PTH-02_evaluation_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
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

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional O(N), certified trig/range reduction; nonorthogonal nondegenerate axes
permitted.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Circle quadrants, ellipse nonconstant speed, FullTurn endpoint bit identity and huge
angles.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
