---
spec_schema_version: 1
id: PTH-08B
parent_id: PTH-08
function: fit_cubic_segments
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.fit_cubic_polyline_envelope_v1_strict
  - path.fit_cubic_polyline_envelope_locked_v1_strict
  - path.fit_cubic_polyline_envelope_tangents_v1_strict
---

# PTH-08B: fit cubic segments

Inherits [PTH-08](PTH-08_editing_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.fit_cubic_polyline_envelope_v1_strict`, `path.fit_cubic_polyline_envelope_locked_v1_strict`, `path.fit_cubic_polyline_envelope_tangents_v1_strict`.

Ordered grouped points/polyline plus corner/locked fields; cubic CoreVerbs PathSet and associated FitReport.

The associated Result uses `photospider.ops.path_fit_report` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

### Parameters and domain

epsilon_geom_px>0,max_segments,max_iterations; endpoint_tangent_policy=chord/explicit; topology allow_change default or reject_unproven.

Input geometry is an M/L/Z CoreVerbs PathSet. The key `path.fit_cubic_polyline_envelope_v1_strict` has `_locked` and `_tangents` arity variants. The optional locked field is UInt8 `[C]`; explicit tangents are Float64 `[C,2,2]` with incoming/outgoing direction on the second axis. Every supplied vertex is interpolated and every source edge produces one cubic; the solver does not minimize the segment count or fit a global smooth spline. Defaults: `epsilon_geom_px=0.05`, `max_segments=1048576`, `max_iterations=64`, `max_refinements=4096`.

## Mathematics

Each backend solver is a separate versioned operator, not a mode or hidden fallback. Shared acceptance requires endpoint/locked hits, corner tangent breaks and continuous bidirectional Hausdorff error including publication. Each operator fixes initialization, numerical solve, reparameterization, split/ties and stopping. Different solvers may return different valid outputs; no minimum-segment promise.

### Solver

For the chord policy, internal controls are the exact one-third/two-third chord points, each published with RN64. For the explicit policy, nonzero supplied endpoint directions are normalized mathematically and the initial handle length is `min(edge_length/3, epsilon_geom_px/4)`. Both handles are halved after each unsuccessful attempt; there is no reparameterization and no alternate solver.

Acceptance is evaluated on the published controls: the maximum exact squared distance of those controls to the source closed segment must not exceed `epsilon_geom_px^2`. Then `RU64(sqrt(max_squared_distance))` certifies the continuous bidirectional Hausdorff error including coordinate publication. Explicit nondegenerate-edge derivatives must have exact zero cross product and strictly positive dot product with the requested directions. Unrepresentable direction constraints or exhausted quality attempts return `InvalidQuality`.

A nonzero locked marker also preserves a source geometric corner. For corner analysis only, consecutive zero-length edges (including across the closed seam) are skipped, and a marker in such a run applies to its two nonzero adjacent edges. A source corner has incoming and outgoing directions that are not positively collinear (exact cross zero and dot positive define positive collinearity). The final cubic's one-sided geometric direction is its first nonzero endpoint control difference, so a zero first derivative does not erase a direction. If the two published directions become positively collinear the call fails with `InvalidQuality`. Open endpoints and all-zero runs have no two-sided corner. The original rows and zero-length edges remain in the output.

`topology=reject_unproven` accepts only the zero-error geometric-identity certificate (same directed geometric path and incidence, not necessarily the same controls, parameterization or speed). Other accepted fits report unverified topology.

## Behavior

Support class: complete input. Exhausting the quality attempts never returns an uncertified best guess.

## Acceptance

Exact line fixture, locked corners, between-sample overshoot rejection, topology policy and same-version bit identity. Endpoint-only helper is not a fitter/certificate.
