---
spec_schema_version: 1
id: GEN-08C
parent_id: GEN-08
function: poisson_rejection_points
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_workflow_keys:
  - generation.poisson_rejection_points_v1_strict
---

# GEN-08C: poisson rejection points

Inherit [GEN-08](GEN-08_point_distributions_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
numerical output bits and discrete decisions are fixed. The report ObjectId field has the Run-local exception specified by GEN-08.

## Interface and representation

bounds[4], positive min_distance[1]; points Result and associated termination
diagnostic.

## Parameters and domain

candidate_count K=0..2^31-1; seed/stream; max_count normal target, constructor default
1048576; max_count may be zero, with P=max(1,max_count) and the DynamicPoints v1 valid-prefix/padding rules.

## Mathematical specialization

Process domain-4 candidates in ordinal order. Publish candidates to Float64 and accept
iff all squared distances to earlier accepted points are >=r^2. IDs follow acceptance.
Reaching max_count succeeds with count_reached; exhausting K succeeds with
candidates_exhausted. K=0 gives empty. Candidate i uses x=i and y=channel=frame=draw=0, with caller seed/stream. Halfopen53 values from words 0/1 determine x/y in bounds; words 2/3 are unused.

The public member expands into generation followed by report binding under [GEN-08](GEN-08_point_distributions_contract.md). Export the same complete points value consumed by the binder; no post-binding point copy or partial projection preserves that identity.

## Dependencies, resources and failures

Computation scope: **complete input in a Run-shared stage**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, following the declared dependencies and static structure checks.

complete-input Result; reference O(K*M), exact spatial index allowed; independent hard budgets
fail.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural errors and operational NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Minimum distances, empty K, radius above canvas diagonal, accepted-prefix stability,
normal target completion versus hard failure.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
