---
spec_schema_version: 1
id: GEN-03F
parent_id: GEN-03
function: ellipse_sdf
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.ellipse_sdf_v1_strict
  - generation.ellipse_sdf_v1_accelerated_apple_silicon
  - generation.ellipse_sdf_v1_accelerated_x86_64
---

# GEN-03F: ellipse sdf

Inherit [GEN-03](GEN-03_basic_shapes_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

center[2], positive radii[2]; distance[H,W] in pixels.

## Parameters and domain

canvas, dtype default Float64, max_refinements default 4096.

## Mathematical specialization

Signed minimum Euclidean distance to c+(rx*cos(theta),ry*sin(theta)); implicit ellipse
test determines sign. Boundary +0; center=-min(rx,ry); circle reduces to norm(p-c)-r.
Isolate all relevant minima.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional queries with certified global minimum and rounding.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Ellipse (2,1) has center -1 and distance 1 at (3,0); interior axis points can have
off-axis closest points.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
