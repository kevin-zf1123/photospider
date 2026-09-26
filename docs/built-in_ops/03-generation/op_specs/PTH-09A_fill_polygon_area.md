---
spec_schema_version: 1
id: PTH-09A
parent_id: PTH-09
function: fill_polygon_area
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.fill_polygon_area_v1_strict
  - path.fill_polygon_area_v1_accelerated_apple_silicon
  - path.fill_polygon_area_v1_accelerated_x86_64
---

# PTH-09A: fill polygon area

Inherit [PTH-09](PTH-09_fill_contract.md), [GEN common](GEN_common_contract.md), and the
applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

M/L/Z PathSet; raw scalar coverage[H,W] Float32/64.

## Parameters and domain

canvas/dtype; fill_rule nonzero default or evenodd; self-crossing, holes and overlaps
permitted.

## Mathematical specialization

RN_T(area(pixel intersect regularized_fill(path,rule))). Inputs are exact dyadic
coordinates, intersections/areas rational. Fill open contours with an implicit
consumption-only closing edge. Set coverage, not contour alpha-over; zero-area edges
contribute nothing. Final[0,1] without error-hiding clamp.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional pixels with complete path validation/index; reference vertical-slab integration
O(area(Q)*E^2).

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Half pixel, unit square, bow-tie1/2, duplicate contours nonzero1/evenodd0, holes/shared
edges/tiny rational areas.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
