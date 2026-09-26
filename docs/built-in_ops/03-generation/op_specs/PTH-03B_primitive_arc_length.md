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

## Parameters and domain

Same budgets; Bezier/arc/Hermite/nonperiodic spline; split knot spans and subpath
discontinuities.

## Mathematical specialization

Orthogonal equal-length arc axes give norm(u)*abs(sweep); other ellipses integrate
speed. Hermite may use exact rational Bezier conversion. Integrate nonzero spline knot
spans, not discontinuous jumps. Share deterministic cumulative/selection rules.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole; certified directed integration or valid control bounds, not ordinary quadrature
estimates.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Unit circle2pi, equal-axis ellipse reduction, degree1 length5, degree0 length0 and
Hermite equivalence.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
