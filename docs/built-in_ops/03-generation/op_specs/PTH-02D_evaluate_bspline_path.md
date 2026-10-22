---
spec_schema_version: 1
id: PTH-02D
parent_id: PTH-02
function: evaluate_bspline_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.evaluate_bspline_path_v1_strict
  - path.evaluate_bspline_path_v1_accelerated_apple_silicon
  - path.evaluate_bspline_path_v1_accelerated_x86_64
---

# PTH-02D: evaluate bspline path

Inherit [PTH-02](PTH-02_evaluation_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Spline records and associated controls/knots/weights,segment_indices[N],t[N]; four
evaluation outputs.

## Parameters and domain

degree0..16; records[S,5]=(degree,first_control,control_count,first_knot,weighted),
controls[U,2], knots[K] and an explicit weights[U] input; t in [0,1], no
extrapolation; dtype. For weighted=1, use the positive stored weights. For
weighted=0, every mathematical weight is exactly one regardless of the weights
payload; the explicit input port and its static shape remain required.

The standalone fifth field is distinct from Primitive PathSet's knot_count.
Primitive splines always use their stored weight pool; nonrational Primitive
records carry unit weights in that pool.

## Mathematical specialization

tau=k[d]+t*(k[n]-k[d]); rational Cox-de Boor weighted sum divided by positive weight
sum. Zero-denominator basis terms contribute0. Internal knots use right limits, final
endpoint left limit. Derivative is with respect to t and includes domain-length factor;
rational derivative uses quotient rule. degree0 derivative0.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and the controls required by the mathematics. Complete control reads do not imply runtime payload validation. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output mathematics, following the declared dependencies and static structure checks.

Regional O(N*d^2), valid payloads assumed; no rounding-changing weight normalization.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural errors and operational NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Degree1 polyline, repeated knots, nonclamped final endpoint, common weight scaling and
rational quadratic circular fixture. With degree one, controls (0,0),(2,0),
knots [0,0,1,1], stored weights [1,3] and t=1/2, weighted=0 gives position
(1,0) and derivative (2,0); weighted=1 gives position (3/2,0) and derivative
(3/2,0). The corresponding Primitive record retains the weighted result.
Exercise both flags in one query batch and both input/output float formats.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
