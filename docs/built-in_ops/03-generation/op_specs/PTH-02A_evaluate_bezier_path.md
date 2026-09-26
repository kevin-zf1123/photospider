---
spec_schema_version: 1
id: PTH-02A
parent_id: PTH-02
function: evaluate_bezier_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.evaluate_bezier_path_v1_strict
  - path.evaluate_bezier_path_v1_accelerated_apple_silicon
  - path.evaluate_bezier_path_v1_accelerated_x86_64
---

# PTH-02A: evaluate bezier path

Inherit [PTH-02](PTH-02_evaluation_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

controls[S,d+1,2],segment_indices[N],t[N];
position[N,2],derivative[N,2],tangent[N,2],UInt8 tangent_valid[N].

## Parameters and domain

degree1/2/3; Float32/64 controls/query, explicit output dtype; t in [0,1], positive
Value extents.

## Mathematical specialization

B(t)=sum binom(d,i)*(1-t)^(d-i)*t^i*P_i; derivative=d*sum B_i^(d-1)*(P_(i+1)-P_i). Round
complete component expressions once. Endpoints copy/convert controls. Interior exact
zero preserves -0 only when every control in that component is -0. Tangent validity uses
exact derivative, not rounded derivative.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional queries with complete control/index structural validation; reference O(N*d^2).

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

3-4-5 line midpoint/derivative/unit tangent, cubic endpoint derivatives, exact-zero
tangent and reversal identity.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
