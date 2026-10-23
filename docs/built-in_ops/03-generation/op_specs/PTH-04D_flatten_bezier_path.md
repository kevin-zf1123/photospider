---
spec_schema_version: 1
id: PTH-04D
parent_id: PTH-04
function: flatten_bezier_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.flatten_bezier_path_v1_strict
---

# PTH-04D: flatten bezier path

Inherit [PTH-04](PTH-04_resampling_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Core/Bezier PathSet; Polyline PathSet and optional associated parameter samples.

## Parameters and domain

epsilon_geom_px>0 default0.05, explicit preview0.25; max_depth24,max_segments;
attribute_policy; topology allow_change default or reject_unproven.

## Mathematical specialization

Exact t=1/2 de Casteljau, DFS left first. Bound parameter-matched curve/chord deviation
by differences between degree-d controls and degree-elevated endpoint chord controls.
Add maximum RN64 endpoint displacement before accepting leaf. Shared vertices once,
closed Z without duplicate M. Never use perpendicular flatness alone.

## Runtime implementation

`path_flatten.cpp` publishes `path`, `report`, `samples` and `source_map` as explicit associated outputs. Exact dyadic subdivision parameters are encoded in `path_source_map`; its limb capacity follows the configured subdivision depth. The parameter-matched bound includes publication displacement and is checked on each accepted leaf. `topology=reject_unproven` currently accepts only the sufficient exact geometric-identity certificate; a nonzero bound returns `InvalidQuality`. This is a conservative success domain, not a general topology solver.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input structural Result; O(leaves*d^2), bounded depth and count. Strict topology request
needs proof or fails.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Straight line, collinear reversal, certified total error including publication; sampled
checks supplement the control bound, not replace it.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
