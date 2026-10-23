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

Inherit [PTH-08](PTH-08_editing_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Ordered grouped points/polyline plus corner/locked fields; cubic CoreVerbs PathSet and
associated FitReport.

The associated Result uses `photospider.ops.path_fit_report` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

## Parameters and domain

epsilon_geom_px>0,max_segments,max_iterations; endpoint_tangent_policy=chord/explicit;
topology allow_change default or reject_unproven.

## Mathematical specialization

Each backend solver is a separate versioned operator, not a mode or hidden fallback.
Shared acceptance requires endpoint/locked hits, corner tangent breaks and continuous
bidirectional Hausdorff error including publication. Each operator fixes initialization,
numerical solve, reparameterization, split/ties and stopping. Different solvers may
return different valid outputs; no minimum-segment promise.

## Runtime implementation

The frozen solver is `path.fit_cubic_polyline_envelope_v1_strict`, with `_locked` and `_tangents` arity variants. Input geometry is an M/L/Z CoreVerbs PathSet. The optional locked field is UInt8 `[C]`; explicit tangents are Float64 `[C,2,2]`, with incoming/outgoing direction in the second axis. Every supplied vertex is interpolated and every source edge produces one cubic. This solver does not minimize the segment count or fit a global smooth spline.

For chord policy, internal controls are the exact one-third/two-third chord points, each published with RN64. For explicit policy, nonzero supplied endpoint directions are normalized mathematically and the initial handle is `min(edge_length/3, epsilon_geom_px/4)`. Both handles are halved after an unsuccessful attempt; no reparameterization or hidden alternate solver occurs. Defaults are `epsilon_geom_px=0.05`, `max_segments=1048576`, `max_iterations=64` and `max_refinements=4096`.

Acceptance is evaluated on the actual published controls: the maximum exact squared distance of those controls to the source closed segment must not exceed `epsilon_geom_px^2`. The convex capsule bounds curve-to-segment distance; endpoint interpolation and continuity of projection bound segment-to-curve distance. Thus `RU64(sqrt(max_squared_distance))` certifies continuous bidirectional Hausdorff error, including coordinate publication. Explicit nondegenerate-edge derivatives must have exact zero cross product and strictly positive dot product with the requested directions. Unrepresentable direction constraints or exhausted quality attempts return `InvalidQuality`.

A nonzero locked marker also preserves a source geometric corner. For corner analysis only, skip consecutive zero-length edges, including the closed seam; any marker in such a run applies to its two nonzero adjacent edges. A source corner has incoming/outgoing directions that are not positively collinear (exact cross zero and dot positive defines positive collinearity). The final cubic's one-sided geometric direction is its first nonzero endpoint control difference, so a zero first derivative does not erase a geometric direction. If the two published directions become positively collinear, the call fails with `InvalidQuality`. Open endpoints and all-zero runs have no two-sided corner. The original rows and zero-length edges remain in the output.

`topology=reject_unproven` accepts only the zero-error geometric-identity certificate. Identity here means the same directed geometric path and incidence, not the same controls, parameterization or speed. Other accepted fits report unverified topology.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input; candidate fitting and continuous verification charged independently;
the frozen envelope solver and its continuous verifier share no sampled acceptance shortcut. Budget exhaustion cannot return an
uncertified best guess.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Exact line fixture, locked corners, between-sample overshoot rejection, topology policy
and same-version bit identity. Endpoint-only helper is not a fitter/certificate.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
