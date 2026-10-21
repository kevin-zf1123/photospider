---
spec_schema_version: 1
id: NOI-05B
parent_id: NOI-05
function: nearest_point_distances
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.nearest_point_distances_v1_strict
  - noise.nearest_point_distances_v1_accelerated_apple_silicon
  - noise.nearest_point_distances_v1_accelerated_x86_64
---

# NOI-05B: nearest point distances

Inherit [NOI-05](NOI-05_cellular_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

coordinates[S...,2], [DynamicPoints v1](GEN-08_point_distributions_contract.md); f1,f2 and nearest_id.

## Parameters and domain

dtype; at least two distinct IDs; coincident positions allowed; valid PointSet
association.

## Mathematical specialization

Treat published positions as exact points. Sort by (squared distance,id), return RN_T
square roots of first two distances and first ID. Coincident distinct IDs can give
F1=F2=0; rows do not decide ties.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and the controls required by the mathematics. Complete control reads do not imply runtime payload validation. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output mathematics, following the declared dependencies and static structure checks.

Regional coordinate queries with complete point control set; O(count(Q)*M), equivalent
BVH allowed.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural errors and operational NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Bisector tie, coincident sites, row permutation invariance, at least two valid points with distinct IDs as an unchecked premise.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
