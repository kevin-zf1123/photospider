---
spec_schema_version: 1
id: GEN-08C
parent_id: GEN-08
function: poisson_rejection_points
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
workflow_helpers:
  - ps::generation::poisson_rejection_points
operation_keys:
  - generation.poisson_rejection_candidates_v1_strict
  - generation.bind_point_generation_report_v1
---

# GEN-08C: poisson rejection points

Inherit [GEN-08](GEN-08_point_distributions_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
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

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input Result; reference O(K*M), exact spatial index allowed; independent hard budgets
fail.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## CPU implementation and public workflow

`ps::generation::poisson_rejection_points` in
`photospider/ops/generation.hpp` transactionally appends
`generation.poisson_rejection_candidates_v1_strict` and
`generation.bind_point_generation_report_v1`. It returns `GeneratedPoints`
references named `points` and `report`; callers export both complete values.
The producer takes Float32/64 bounds and minimum distance, with required Int64
`candidate_count`, `seed`, `stream`, optional `max_count=1048576` and
`max_work=INT64_MAX`. Its internal outputs are `points` and `stop_reason`.

Candidate mapping is exact through the final RN64 and half-open-bound correction.
A conservative binary64 interval first compares squared distances; only proven
comparisons are accepted, and uncertain cases use exact rational comparison.
This changes neither equality acceptance nor candidate order. The traversal remains
O(K*M) with O(P) admitted scratch and complete canonical padding. Work, memory and
cancellation failures do not publish a successful prefix.

The binder reads all three point members and the raw reason, binds the actual input
ObjectId and declares `deterministic=false`. No GPU implementation is registered.
The public helper is a workflow constructor, not an additional primitive key.

## Independent fixtures and acceptance

Minimum distances, empty K, radius above canvas diagonal, accepted-prefix stability,
normal target completion versus hard failure.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
