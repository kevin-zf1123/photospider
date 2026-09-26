---
spec_schema_version: 1
id: GEN-07B
parent_id: GEN-07
function: triangle_mesh_gradient
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# GEN-07B: triangle mesh gradient

Inherit [GEN-07](GEN-07_mesh_gradients_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

vertices[V,2], triangles[F,3] Int64, controls[V,C], C>=1; values[H,W,C] and UInt8
valid[H,W].

## Parameters and domain

canvas/dtype; nonzero triangle areas; no interior overlaps, shared edges/vertices
allowed; outside=zero+valid0.

## Mathematical specialization

Exact orientation locates triangles; shared-boundary ownership chooses smallest face
row. Exact barycentric weights form each complete component expression before RN. Shared
vertices share control vectors; outside components are +0. No color/alpha inference.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional pixels with complete topology/control validation; indexes may only eliminate
proven irrelevant faces.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Shared-edge continuity, reversed triangle orientation, overlap rejection, outside
validity and arbitrary C.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
