---
spec_schema_version: 1
id: PTH-03B
parent_id: PTH-03
function: primitive_arc_length
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.primitive_arc_length_v1_strict
  - path.primitive_arc_length_v1_accelerated_apple_silicon
  - path.primitive_arc_length_v1_accelerated_x86_64
---

# PTH-03B: primitive arc length

Inherit [PTH-03](PTH-03_arc_length_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Primitives PathSet; length and table as PTH-03A.

The associated Result uses `photospider.ops.path_arclength_table` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

## Parameters and domain

Same budgets; Bezier/arc/Hermite/nonperiodic spline and explicit Close; split knot spans and subpath
discontinuities.

## Mathematical specialization

Orthogonal equal-length arc axes give norm(u)*abs(sweep); other ellipses integrate
speed. Hermite may use exact rational Bezier conversion. Integrate nonzero spline knot
spans, not discontinuous jumps. Close length is the norm of the difference of its mathematical endpoints, without an intermediate Float64 endpoint. Share deterministic cumulative/selection rules.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input; certified directed integration or valid control bounds, not ordinary quadrature
estimates.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Unit circle2pi, equal-axis ellipse reduction, degree1 length5, degree0 length0 and
Hermite equivalence.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
