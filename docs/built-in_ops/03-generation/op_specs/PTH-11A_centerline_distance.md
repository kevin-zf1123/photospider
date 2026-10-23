---
spec_schema_version: 1
id: PTH-11A
parent_id: PTH-11
function: centerline_distance
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.centerline_distance_v1_strict
  - path.centerline_distance_v1_accelerated_apple_silicon
  - path.centerline_distance_v1_accelerated_x86_64
---

# PTH-11A: centerline distance

Inherit [PTH-11](PTH-11_distance_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
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

## Runtime implementation

`path_distance.cpp` uses exact squared-distance polynomials, Sturm isolation of all stationary roots, and resultant/interval comparisons in `algebraic.hpp`. It compares mathematical candidate distances and the specified provenance/t ties before independently rounding distance, parameter and closest point. The strict and two architecture-named CPU profiles share this certified solver. Failure to isolate, order or round within finite resources is an operational/certification failure, not an approximate nearest point.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional pixels/full path controls; certified root isolation/global comparison; BVH
cannot discard equal-distance ties.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

3-4-5, on-curve zero, U-curve multiple minima, lexicographic tie, empty truncation and
rounded-t reconstruction difference.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
