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

Inherits [GEN-08](GEN-08_point_distributions_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `ps::generation::poisson_rejection_points`, `generation.poisson_rejection_candidates_v1_strict`, `generation.bind_point_generation_report_v1`.

bounds[4], positive min_distance[1]; points Result and associated termination diagnostic.

### Parameters and domain

candidate_count K=0..2^31-1; seed/stream; max_count normal target, constructor default 1048576; max_count may be zero, with P=max(1,max_count) and the DynamicPoints v1 valid-prefix/padding rules.

Public SDK helper, declared in `photospider/ops/generation.hpp`:

```cpp
struct GeneratedPoints final { WorkflowNodeOutput points, report; };
Result<GeneratedPoints> ps::generation::poisson_rejection_points(
    WorkflowDocument* document, WorkflowInput bounds,
    WorkflowInput min_distance, Parameters parameters);
```

The helper appends `generation.poisson_rejection_candidates_v1_strict`, whose outputs are `points` and `stop_reason`, and then `generation.bind_point_generation_report_v1`. `GeneratedPoints::points` is the same complete points value the binder consumed, and `report` is the bound report; callers export both complete values. Inputs: `bounds` Float32/64 `[4]`, `min_distance` Float32/64 `[1]`. Required Int64 parameters: `candidate_count`, `seed`, `stream`. Optional Int64: `max_count` (default 1048576). The binder has `deterministic=false` (see [GEN-08](GEN-08_point_distributions_contract.md)).

## Mathematics

Process domain-4 candidates in ordinal order. Publish candidates to Float64 and accept iff all squared distances to earlier accepted points are >=r^2. IDs follow acceptance. Reaching max_count succeeds with count_reached; exhausting K succeeds with candidates_exhausted. K=0 gives empty. Candidate i uses x=i and y=channel=frame=draw=0, with caller seed/stream. Halfopen53 values from words 0/1 determine x/y in bounds; words 2/3 are unused.

The public member expands into generation followed by report binding under [GEN-08](GEN-08_point_distributions_contract.md). Export the same complete points value consumed by the binder; no post-binding point copy or partial projection preserves that identity.

## Behavior

Support class: complete input.

## Acceptance

Minimum distances, empty K, radius above canvas diagonal, accepted-prefix stability, normal target completion versus hard failure.
