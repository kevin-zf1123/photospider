---
spec_schema_version: 1
id: PTH-10C
parent_id: PTH-10
function: stroke_outline_flatten
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.stroke_outline_flatten_v1_strict
---

# PTH-10C: stroke outline flatten

Inherit [PTH-10](PTH-10_stroke_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Explicit constant or variable-round stroke recipe; closed CoreVerbs outline and
GeometryReport.

## Parameters and domain

epsilon_outline_px>0 default0.05; max_segments,attribute_policy; topology allow_change
default or reject_unproven.

## Mathematical specialization

Construct the exact exposed stroke boundary then deterministic equal-angle DFS
subdivision of arcs, bounding chord sagitta plus RN64 endpoint displacement. Straight
edges preserved, contours canonicalized. Default reports unverified topology; strict
policy proves required feature/face relations or fails. Output polygon area is not
original curved-stroke area.

## Runtime implementation

`path_stroke.cpp` and `CircularArrangement` share the exact union with PTH-10A/B. `width_mode=constant` consumes width `[1]`; `variable_round` consumes widths `[C]`. The topology parameter is `topology_policy`, default `allow_change`.

The v1 bound accumulation is frozen: independently round every exact arc sagitta and endpoint displacement upward to Float64, retain each stage's maximum, then compare the exact rational sum of these published upper bounds with `epsilon_outline_px`. Report approximation and publication stages separately. This avoids requiring symbolic equality between deeply nested symmetric radical bounds, without replacing any geometric predicate by a tolerance. Subdivision remains deterministic equal-angle, left-first. `topology_policy=reject_unproven` currently requires zero approximation and publication bounds (geometric identity), otherwise `InvalidQuality`.

Defaults include `max_depth=52`, `max_segments=1048576`, `max_events=1048576`, `max_expression_nodes=262144`, `max_refinements=4096` and 128 MiB algorithm scratch. Exhausting any independent hard budget fails the entire call.

The publication maximum is global over the vertices of all final exposed contours, including straight-edge endpoints. Cache each exact endpoint's RN64 coordinates and RU64 displacement once. If publication alone exceeds the requested bound, fail with `InvalidQuality`; a straight edge does not bypass this check. Refinement compares the maximum final-leaf sagitta plus this global publication maximum as an exact rational sum. Newly retained split vertices can increase publication error, so recheck earlier leaves and refine again in deterministic left-first order until the global sum fits. Count both initial edges and added splits against `max_segments`; no prefix is published on failure.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input geometry Result; arrangement and output work bounded, no implicit width
modification or unreported quality degradation.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Butt line rectangle, circle outline bound, narrow-hole strict failure and permitted
geometric difference from direct exact coverage. The diagonal `(0,0)->(1,1)`, width 1, butt cap fails at `epsilon_outline_px=1e-20` and succeeds at `5e-17`. Include multiple translated round contours whose maximum sagitta and maximum publication displacement occur at different vertices; require the exact sum of the reported stage bounds to satisfy the request. Check initial-edge, split and depth limits.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
