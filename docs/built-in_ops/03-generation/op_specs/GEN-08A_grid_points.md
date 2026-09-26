---
spec_schema_version: 1
id: GEN-08A
parent_id: GEN-08
function: grid_points
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.grid_points_v1_strict
---

# GEN-08A: grid points

Inherit [GEN-08](GEN-08_point_distributions_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

bounds[4]; DynamicPoints Result, Float64 positions and ordinal IDs.

## Parameters and domain

nx,ny>=1; positive area bounds; checked nx*ny against hard capacity; no seed.

## Mathematical specialization

Point (j,i) is RN64(xmin+(i+1/2)*(xmax-xmin)/nx), similarly y. ID=j*nx+i. Enforce
representable half-open source cells using the explicit lattice endpoint rule; reject
duplicate published points.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole complete Result, O(nx*ny).

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

2x2 grid in [0,2]^2 gives centers 0.5/1.5; one-axis singleton; unrepresentable cell
rejection.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
