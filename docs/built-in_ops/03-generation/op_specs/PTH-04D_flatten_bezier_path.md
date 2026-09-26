---
spec_schema_version: 1
id: PTH-04D
parent_id: PTH-04
function: flatten_bezier_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.flatten_bezier_path_v1_strict
---

# PTH-04D: flatten bezier path

Inherit [PTH-04](PTH-04_resampling_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Core/Bezier PathSet; Polyline PathSet and optional associated parameter samples.

## Parameters and domain

epsilon_geom_px>0 default0.05, explicit preview0.25; max_depth24,max_segments;
attribute_policy; topology allow_change default or reject_unproven.

## Mathematical specialization

Exact t=1/2 de Casteljau, DFS left first. Bound parameter-matched curve/chord deviation
by differences between degree-d controls and degree-elevated endpoint chord controls.
Add maximum RN64 endpoint displacement before accepting leaf. Shared vertices once,
closed Z without duplicate M. Never use perpendicular flatness alone.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole structural Result; O(leaves*d^2), bounded depth and count. Strict topology request
needs proof or fails.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Straight line, collinear reversal, certified total error including publication; sampled
checks supplement the control bound, not replace it.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
