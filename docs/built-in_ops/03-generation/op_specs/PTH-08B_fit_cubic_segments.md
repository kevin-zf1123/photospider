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

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole; candidate fitting and continuous verification charged independently;
solver/verifier must be frozen before registration. Budget exhaustion cannot return an
uncertified best guess.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Exact line fixture, locked corners, between-sample overshoot rejection, topology policy
and same-version bit identity. Endpoint-only helper is not a fitter/certificate.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
