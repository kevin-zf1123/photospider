---
spec_schema_version: 1
id: PTH-08A
parent_id: PTH-08
function: simplify_polyline
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.simplify_polyline_v1_strict
---

# PTH-08A: simplify polyline

Inherit [PTH-08](PTH-08_editing_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

M/L/Z PathSet and optional locked-vertex fields; simplified PathSet.

## Parameters and domain

epsilon_geom_px>=0; topology allow_change default or reject_unproven; open endpoints
locked; closed original M plus farthest vertex anchors, ties by source row.

## Mathematical specialization

Candidate chord parameterizes source vertices by original cumulative arc length. Check
corresponding vertex distances<=epsilon; piecewise linearity supplies a continuous
bidirectional Hausdorff sufficient bound. Otherwise split at largest-error vertex, tie
smallest row. Zero epsilon may remove forward collinear points but not reversals. Strict
topology additionally proves intersection/face relations.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole reference O(V^2), exact algebraic length comparisons and fixed ties; not ordinary
perpendicular Douglas-Peucker.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Forward collinear simplification, reversal preservation, locked points, continuous bound
and strict small-hole proof failure.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
