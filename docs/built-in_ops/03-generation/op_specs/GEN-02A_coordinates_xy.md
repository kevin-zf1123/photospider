---
spec_schema_version: 1
id: GEN-02A
parent_id: GEN-02
function: coordinates_xy
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.coordinates_xy_v1_strict
  - generation.coordinates_xy_v1_accelerated_apple_silicon
  - generation.coordinates_xy_v1_accelerated_x86_64
---

# GEN-02A: coordinates xy

Inherit [GEN-02](GEN-02_coordinate_grid_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

No Value inputs; coordinates[H,W,2] Float32/64, component order xy.

## Parameters and domain

canvas, dtype default Float64; space=pixel/normalized_edge, default pixel.

## Mathematical specialization

pixel returns RN_T(origin_x+x+1/2), RN_T(origin_y+y+1/2). normalized_edge returns
RN_T((x+1/2)/W), RN_T((y+1/2)/H). Construct exact rational coordinates before rounding.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

O(area(Q)); no upstream samples.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

W=4 gives x=(1/8,3/8,5/8,7/8); 1x1 normalized center=(0.5,0.5); origin=(10,-2) gives
first center=(10.5,-1.5).

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
