---
spec_schema_version: 1
id: PTH-09A
parent_id: PTH-09
function: fill_polygon_area
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.fill_polygon_area_v1_strict
  - path.fill_polygon_area_v1_accelerated_apple_silicon
  - path.fill_polygon_area_v1_accelerated_x86_64
---

# PTH-09A: fill polygon area

Inherit [PTH-09](PTH-09_fill_contract.md), [GEN common](GEN_common_contract.md), and the
applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
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

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional pixels with the complete path support/index required by integration; no path
payload validation. Reference vertical-slab integration
O(area(Q)*E^2).

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Half pixel, unit square, bow-tie1/2, duplicate contours nonzero1/evenodd0, holes/shared
edges/tiny rational areas.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
