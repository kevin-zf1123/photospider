---
spec_schema_version: 1
id: PTH-11B
parent_id: PTH-11
function: fill_region_sdf
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.fill_region_sdf_v1_strict
  - path.fill_region_sdf_v1_accelerated_apple_silicon
  - path.fill_region_sdf_v1_accelerated_x86_64
---

# PTH-11B: fill region sdf

Inherit [PTH-11](PTH-11_distance_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Polyline PathSet; distance[H,W]; optional closest/provenance requires formal associated
schema.

## Parameters and domain

canvas/dtype/fill_rule; optional positive finite max_distance; source curves explicitly
flattened.

## Mathematical specialization

Use regularized filled region and its exposed boundary. Distance to that boundary with
inside negative, boundary +0; remove internal shared/overlap edges. Optional signed
magnitude clipping. Empty region returns +max only when supplied, otherwise NoSolution.

## Runtime implementation

`path_boolean.cpp` selects the regularized region's exposed boundary with the exact rational `PolygonArrangement` backend, then performs exact segment projection and certified distance rounding. Internal overlap edges are removed before distance queries. Three CPU profiles are registered; only distance is a public output of this v1 member.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional queries with exact global boundary arrangement; reference O(area(Q)*boundary).

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Rectangle interior, adjacent union shared edge not zero, holes, repeated evenodd
contours empty and signed clipping.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
