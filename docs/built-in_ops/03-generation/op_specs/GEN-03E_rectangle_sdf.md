---
spec_schema_version: 1
id: GEN-03E
parent_id: GEN-03
function: rectangle_sdf
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.rectangle_sdf_v1_strict
  - generation.rectangle_sdf_v1_accelerated_apple_silicon
  - generation.rectangle_sdf_v1_accelerated_x86_64
---

# GEN-03E: rectangle sdf

Inherit [GEN-03](GEN-03_basic_shapes_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

center[2], positive half_size[2]; distance[H,W] in pixels.

## Parameters and domain

canvas, dtype default Float64; zero-size regions rejected.

## Mathematical specialization

q=abs(p-center)-half_size; RN_T(norm2(max(q,0))+min(max(qx,qy),0)), where norm2 denotes
Euclidean length. Interior negative, boundary +0.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

O(area(Q)).

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Unit half-size gives center -1 and distance sqrt(2) at (2,2).

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
