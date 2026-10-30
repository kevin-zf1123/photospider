---
spec_schema_version: 1
id: GEN-08D
parent_id: GEN-08
function: bridson_points
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
workflow_helpers:
  - ps::generation::bridson_points
operation_keys:
  - generation.bridson_candidates_v1_strict
  - generation.bind_point_generation_report_v1
---

# GEN-08D: bridson points

Inherits [GEN-08](GEN-08_point_distributions_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `ps::generation::bridson_points`, `generation.bridson_candidates_v1_strict`, `generation.bind_point_generation_report_v1`.

bounds[4], positive min_distance[1]; points Result and termination diagnostic.

### Parameters and domain

k=1..256 default 30; seed/stream, frame=0; max_count is a nonnegative normal target with P=max(1,max_count); independent max_iterations/work/memory limits. max_iterations is in 0..2^31-1.

Public SDK helper, declared in `photospider/ops/generation.hpp`:

```cpp
Result<GeneratedPoints> ps::generation::bridson_points(
    WorkflowDocument* document, WorkflowInput bounds,
    WorkflowInput min_distance, Parameters parameters);
```

The helper appends `generation.bridson_candidates_v1_strict` and `generation.bind_point_generation_report_v1` and returns the same `GeneratedPoints` pair as GEN-08C. Required Int64 parameters: `seed`, `stream`. Optional Int64: `k` (default 30), `max_count` (default 1048576), `max_iterations` (default 2097153), `max_refinements` (default 4096).

Failures: an exhausted iteration or refinement limit returns `OperationFailed` with domain code `0x47454e0001`; an unrepresentable candidate returns `OperationFailed` with domain code `0x47454e0004`. Neither publishes a successful short prefix.

## Mathematics

Domain-5 first point lies in bounds. Always choose earliest active point. Try up to k candidates with rho=r*sqrt(1+3u), theta=2*pi*v; accept first legal candidate and append it while retaining parent, otherwise remove parent. Exact distances use published points. count_reached and active_exhausted are successful distinct stops. Each candidate coordinate is one rounding of the complete real expression: `RN64(parent_x + r*sqrt(1+3*u)*cos(2*pi*v))` and `RN64(parent_y + r*sqrt(1+3*u)*sin(2*pi*v))`, with no rounded rho, angle, trigonometric value or offset intermediate. Exact multiples of one eighth turn use the corresponding exact algebraic values. Initial x/y use halfopen53 from words 0/1; candidate u/v use the same word selection, leaving words 2/3 unused. The initial point uses x=y=0. Iteration i, starting at zero, and attempt a use x=i+1,y=a with channel=frame=draw=0. Increment i after accepting one candidate or exhausting the current earliest active point's attempts. An exhausted iteration budget fails only when another iteration is required.

The public member expands into generation followed by report binding under [GEN-08](GEN-08_point_distributions_contract.md). Export the same complete points value consumed by the binder; no post-binding point copy or partial projection preserves that identity.

## Behavior

Support class: complete input.

## Acceptance

Reproducible FIFO order, in-bounds/pairwise separation, one-point target success, active exhaustion and independent iteration failure; no maximal-set claim.
