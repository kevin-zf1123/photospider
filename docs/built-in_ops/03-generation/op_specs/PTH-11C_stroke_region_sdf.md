---
spec_schema_version: 1
id: PTH-11C
parent_id: PTH-11
function: stroke_region_sdf
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.stroke_region_sdf_v1_strict
  - path.stroke_region_sdf_v1_accelerated_apple_silicon
  - path.stroke_region_sdf_v1_accelerated_x86_64
---

# PTH-11C: stroke region sdf

Inherit [PTH-11](PTH-11_distance_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Path and explicit width recipe supported by PTH-10A/B; distance[H,W].

## Parameters and domain

canvas/dtype,cap/join/miter or variable-round rules, optional max_distance.

## Mathematical specialization

Distance to the actual exposed boundary of the exact stroke union, negative inside.
Isolated round point is disk SDF. Min of component SDFs is not a general exact union
distance inside overlaps. Using polygonized outlines requires an explicit
PTH-10C->PTH-11B workflow with its geometry error.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional queries/global stroke boundary controls; certified line/arc nearest point.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Disk center/boundary/outside, butt ends, intersecting bands' true boundary and empty
zero-width region.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
