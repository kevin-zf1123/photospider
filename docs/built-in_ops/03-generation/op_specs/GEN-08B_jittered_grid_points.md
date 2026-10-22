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

Domain 3 addresses cell (i,j), channel/draw=0; use halfopen53 from words 0/1 for x/y respectively; words 2/3 are unused.
Position=cell_lower+cell_size*(1/2+jitter*(U-1/2)), RN64 and the specified half-open-cell endpoint correction; representable cell interiors and distinct output points are caller premises.
jitter=0 matches grid bits; seed remains identity.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input Result, O(nx*ny); use the DynamicPoints v1 member directory and CompleteBundle publication.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

One point per cell, jitter zero equivalence, reproducibility; changing nx changes cells
and need not preserve old positions.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
