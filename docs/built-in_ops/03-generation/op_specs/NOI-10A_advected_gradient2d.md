---
spec_schema_version: 1
id: NOI-10A
parent_id: NOI-10
function: advected_gradient2d
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.advected_gradient2d_v1_strict
  - noise.advected_gradient2d_v1_accelerated_apple_silicon
  - noise.advected_gradient2d_v1_accelerated_x86_64
---

# NOI-10A: advected gradient2d

Inherit [NOI-10](NOI-10_temporal_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

coordinates[S...,2],velocity[2],time[1]; values[S...].

## Parameters and domain

dtype and NOI-04B seed/stream/period; base frame fixed0; velocity coordinate
units/second, time seconds.

## Mathematical specialization

Evaluate the unrounded mathematical NOI-04B field at q=coordinates-velocity*time, then
one final RN. No intermediate coordinate RN; positive velocity moves features in
positive direction.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional, O(4*count(Q)); validate advected signed32 corners, no overflow wrapping.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Zero velocity/time identities, sign of advection, cell continuity; no perceptual
antialias guarantee.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
