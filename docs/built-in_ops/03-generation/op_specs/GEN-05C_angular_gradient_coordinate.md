---
spec_schema_version: 1
id: GEN-05C
parent_id: GEN-05
function: angular_gradient_coordinate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.angular_gradient_coordinate_v1_strict
  - generation.angular_gradient_coordinate_v1_accelerated_apple_silicon
  - generation.angular_gradient_coordinate_v1_accelerated_x86_64
---

# GEN-05C: angular gradient coordinate

Inherit [GEN-05](GEN-05_gradient_coordinates_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

center[2], offset_turns[1]; t[H,W].

## Parameters and domain

canvas/dtype; finite offset in turns; center_policy=zero.

## Mathematical specialization

At center output +0. Otherwise q=atan2(dy,dx)/(2*pi)-offset_turns, then
RN_T(q-floor(q)). Exact seam maps to +0; an interior value rounding to 1 remains 1. No
nextDown or second wrap.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

O(area(Q)); certified branch and period classification.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Cardinal directions, offset seam, signed axes and values rounding to the upper endpoint.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
