---
spec_schema_version: 1
id: PTH-06B
parent_id: PTH-06
function: transform_stroke_outline
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# PTH-06B: transform stroke outline

Inherit [PTH-06](PTH-06_transforms_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Closed stroke outline PathSet and affine[2,3]; transformed closed outline.

## Parameters and domain

Nonsingular affine only; no centerline-width propagation.

## Mathematical specialization

Transform the outline geometry, then fill explicitly using PTH-09. A round cap becomes
elliptical under nonuniform scaling; do not scale a raster mask instead. Propagate prior
polygonization error by at most spectral norm(A) and account subsequent publication
error.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole transformed geometry; downstream fill Regional.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Width2 horizontal outline scaled3 in y has height6, unlike centerline-transform then
width2 stroke; reflected orientation.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
