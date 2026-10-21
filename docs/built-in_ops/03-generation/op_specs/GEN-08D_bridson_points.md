---
spec_schema_version: 1
id: GEN-08D
parent_id: GEN-08
function: bridson_points
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_workflow_keys:
  - generation.bridson_points_v1_strict
---

# GEN-08D: bridson points

Inherit [GEN-08](GEN-08_point_distributions_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
numerical output bits and discrete decisions are fixed. The report ObjectId field has the Run-local exception specified by GEN-08.

## Interface and representation

bounds[4], positive min_distance[1]; points Result and termination diagnostic.

## Parameters and domain

k=1..256 default 30; seed/stream, frame=0; max_count is a nonnegative normal target with P=max(1,max_count); independent
max_iterations/work/memory limits. max_iterations is in 0..2^31-1.

## Mathematical specialization

Domain-5 first point lies in bounds. Always choose earliest active point. Try up to k
candidates with rho=r*sqrt(1+3u), theta=2*pi*v; accept first legal candidate and append
it while retaining parent, otherwise remove parent. Exact distances use published
points. count_reached and active_exhausted are successful distinct stops. Candidate trig
must freeze for strict identity. Initial x/y use halfopen53 from words 0/1; candidate u/v use the same word selection, leaving words 2/3 unused. The initial point uses x=y=0. Iteration i, starting at zero, and attempt a use x=i+1,y=a with channel=frame=draw=0. Increment i after accepting one candidate or exhausting the current earliest active point's attempts. An exhausted iteration budget fails only when another iteration is required.

The public member expands into generation followed by report binding under [GEN-08](GEN-08_point_distributions_contract.md). Export the same complete points value consumed by the binder; no post-binding point copy or partial projection preserves that identity.

## Dependencies, resources and failures

Computation scope: **complete input in a Run-shared stage**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, following the declared dependencies and static structure checks.

complete-input Result; exact grid indexing may accelerate without changing acceptance order. Hard
limits fail, never certify saturation.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural errors and operational NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Reproducible FIFO order, in-bounds/pairwise separation, one-point target success, active
exhaustion and independent iteration failure; no maximal-set claim.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
