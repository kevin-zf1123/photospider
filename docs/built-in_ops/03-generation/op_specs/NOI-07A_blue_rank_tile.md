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

Validate permutation 0..N-1. Euclidean-periodic global addressing yields r; return
RN_T((r+1/2)/N), explicitly nextDown(1) if rounded1 and nextUp(0) if rounded0. This rank
mapping is separate from periodic gradient coordinates. Generic lookup requires
structure/content identity; named blue variants additionally require approved
provenance/license/quality.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional output after full resource validation, O(N+area(Q)); charge immutable
snapshots.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Permutation, negative offsets, content mismatch, midpoint grid and no blue-quality
inference from synthetic fixtures.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
