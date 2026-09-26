---
spec_schema_version: 1
id: GEN-08B
parent_id: GEN-08
function: jittered_grid_points
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.jittered_grid_points_v1_strict
---

# GEN-08B: jittered grid points

Inherit [GEN-08](GEN-08_point_distributions_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

bounds[4]; DynamicPoints Result as GEN-08A.

## Parameters and domain

nx,ny>=1, seed/stream; jitter in [0,1] default 1; frame=0.

## Mathematical specialization

Domain 3 addresses cell (i,j), channel/draw=0; use two candidate halfopen53 uniforms.
Position=cell_lower+cell_size*(1/2+jitter*(U-1/2)), RN64 and half-open-cell validation.
jitter=0 matches grid bits; seed remains identity.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole Result, O(nx*ny); packing remains an explicit gate.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

One point per cell, jitter zero equivalence, reproducibility; changing nx changes cells
and need not preserve old positions.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
