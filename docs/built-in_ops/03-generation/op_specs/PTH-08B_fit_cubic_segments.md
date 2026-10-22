---
spec_schema_version: 1
id: PTH-08B
parent_id: PTH-08
function: fit_cubic_segments
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# PTH-08B: fit cubic segments

Inherit [PTH-08](PTH-08_editing_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
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

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input; candidate fitting and continuous verification charged independently;
solver/verifier must be frozen before registration. Budget exhaustion cannot return an
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
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
