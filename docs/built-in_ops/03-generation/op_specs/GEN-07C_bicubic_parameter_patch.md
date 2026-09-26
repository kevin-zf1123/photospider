---
spec_schema_version: 1
id: GEN-07C
parent_id: GEN-07
function: bicubic_parameter_patch
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# GEN-07C: bicubic parameter patch

Inherit [GEN-07](GEN-07_mesh_gradients_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

bounds[4], controls[4,4,C], C>=1; values[H,W,C].

## Parameters and domain

canvas/dtype; outside=pad/reject; one regular parameter patch, no curved-geometry
inverse mapping.

## Mathematical specialization

Use GEN-07A u/v and RN_T(sum_i,j B_i^3(u)*B_j^3(v)*controls[j,i]) per component. All 16
terms form one expression. Shared four boundary controls give C0 continuity across
explicitly assembled patches; no implicit stitching.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

O(16*C*area(Q)); finite control validation.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Constant control vectors, cubic edges, corner hits; interior controls need not be
interpolated through. Fourth component is ordinary numeric data.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
