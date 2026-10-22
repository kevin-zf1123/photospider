---
spec_schema_version: 1
id: NOI-10B
parent_id: NOI-10
function: temporal_perlin3d
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.temporal_perlin3d_v1_strict
  - noise.temporal_perlin3d_v1_accelerated_apple_silicon
  - noise.temporal_perlin3d_v1_accelerated_x86_64
---

# NOI-10B: temporal perlin3d

Inherit [NOI-10](NOI-10_temporal_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

coordinates[S...,2],time[1],time_scale[1],z_offset[1]; values[S...].

## Parameters and domain

dtype; fixed NOI-04A basis, no seed; finite time, positive time_scale in 1/second.

## Mathematical specialization

z=time*time_scale+z_offset exactly; RN_T(Perlin2002(x,y,z)). Time period
is256/time_scale; arbitrary time queries need no accumulated frame state. Spatial
frequency is explicit upstream mapping.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional O(8*count(Q)).

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Slice equivalence and exact periodic coordinates; no aperiodicity/STBN claim.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
