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

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional, O(4*count(Q)); all demanded advected corners fit signed32 as a caller premise; no wrapping sequence is defined outside that domain.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Zero velocity/time identities, sign of advection, cell continuity; no perceptual
antialias guarantee.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
