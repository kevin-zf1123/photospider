---
spec_schema_version: 1
id: PTH-02D
parent_id: PTH-02
function: evaluate_bspline_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.evaluate_bspline_path_v1_strict
  - path.evaluate_bspline_path_v1_accelerated_apple_silicon
  - path.evaluate_bspline_path_v1_accelerated_x86_64
---

# PTH-02D: evaluate bspline path

Inherit [PTH-02](PTH-02_evaluation_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Spline records and associated controls/knots/weights,segment_indices[N],t[N]; four
evaluation outputs.

## Parameters and domain

degree0..16; optional positive weights; t in [0,1], no extrapolation; dtype.

## Mathematical specialization

tau=k[d]+t*(k[n]-k[d]); rational Cox-de Boor weighted sum divided by positive weight
sum. Zero-denominator basis terms contribute0. Internal knots use right limits, final
endpoint left limit. Derivative is with respect to t and includes domain-length factor;
rational derivative uses quotient rule. degree0 derivative0.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional O(N*d^2), full payload validation; no rounding-changing weight normalization.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Degree1 polyline, repeated knots, nonclamped final endpoint, common weight scaling and
rational quadratic circular fixture.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
