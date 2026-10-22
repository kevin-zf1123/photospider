---
spec_schema_version: 1
id: GEN-03F
parent_id: GEN-03
function: ellipse_sdf
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.ellipse_sdf_v1_strict
  - generation.ellipse_sdf_v1_accelerated_apple_silicon
  - generation.ellipse_sdf_v1_accelerated_x86_64
---

# GEN-03F: ellipse sdf

Inherit [GEN-03](GEN-03_basic_shapes_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

center[2], positive radii[2]; distance[H,W] in pixels.

## Parameters and domain

canvas, dtype default Float64, Int64 max_refinements default 4096.

Retain the full positive Int64 refinement limit; it is not reduced modulo 2^32.
Work budgets and available interval precision remain independent bounds. Limits
4096, 2^32, 2^32+1 and INT64_MAX must produce identical bits whenever the same
rounding certificate is reached within 4096 refinements.

## Mathematical specialization

Signed minimum Euclidean distance to c+(rx*cos(theta),ry*sin(theta)); implicit ellipse
test determines sign. Boundary +0; center=-min(rx,ry); circle reduces to norm(p-c)-r.
Isolate all relevant minima.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional queries with certified global minimum and rounding.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Ellipse (2,1) has center -1 and distance 1 at (3,0); interior axis points can have
off-axis closest points.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
