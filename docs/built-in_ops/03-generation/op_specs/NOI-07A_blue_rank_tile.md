---
spec_schema_version: 1
id: NOI-07A
parent_id: NOI-07
function: blue_rank_tile
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# NOI-07A: blue rank tile

Inherit [NOI-07](NOI-07_blue_noise_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

rank[Th,Tw] Int64 and immutable resource descriptor; values[H,W].

## Parameters and domain

canvas,dtype; offset_x/y Int64 default0; N=Th*Tw<=2^40; no implicit random shift.

## Mathematical specialization

The rank payload is a permutation 0..N-1 as an unchecked caller premise. Euclidean-periodic global addressing yields r; return
RN_T((r+1/2)/N), explicitly nextDown(1) if rounded1 and nextUp(0) if rounded0. This rank
mapping is separate from periodic gradient coordinates. Generic lookup requires
structure/content identity; named blue variants additionally require approved
provenance/license/quality.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional lookup without a permutation/content scan, O(area(Q)); charge immutable
snapshots.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Permutation, negative offsets, valid immutable content binding, midpoint grid and no blue-quality
inference from synthetic fixtures.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
