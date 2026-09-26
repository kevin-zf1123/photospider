---
spec_schema_version: 1
id: PTH-11A
parent_id: PTH-11
function: centerline_distance
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.centerline_distance_v1_strict
  - path.centerline_distance_v1_accelerated_apple_silicon
  - path.centerline_distance_v1_accelerated_x86_64
---

# PTH-11A: centerline distance

Inherit [PTH-11](PTH-11_distance_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Core/Bezier PathSet; distance[H,W],closest[H,W,2] Float64,segment_id Int64,local_t
Float64.

## Parameters and domain

canvas/dtype; optional positive finite max_distance clips distance only; closest still
requires true argmin.

## Mathematical specialization

Minimize Euclidean distance over all segments/t. Line projection clamps exact dot ratio;
zero-length t0. Bezier candidates include endpoints and every real stationary root
in[0,1] (quadratic degree3,cubic degree5). Compare exact distances/ties. Round distance,
closest point and t independently. M-only is a point; empty only permits distance-only
positive truncation, otherwise NoSolution.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional pixels/full path controls; certified root isolation/global comparison; BVH
cannot discard equal-distance ties.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

3-4-5, on-curve zero, U-curve multiple minima, lexicographic tie, empty truncation and
rounded-t reconstruction difference.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
