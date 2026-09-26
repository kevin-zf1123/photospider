---
spec_schema_version: 1
id: PTH-02C
parent_id: PTH-02
function: evaluate_hermite_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.evaluate_hermite_path_v1_strict
  - path.evaluate_hermite_path_v1_accelerated_apple_silicon
  - path.evaluate_hermite_path_v1_accelerated_x86_64
---

# PTH-02C: evaluate hermite path

Inherit [PTH-02](PTH-02_evaluation_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
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

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional O(N) exact polynomial reference.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Endpoint derivative hits, straight-line derivatives, exact Hermite/Bezier algebra
agreement, zero tangent validity.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
