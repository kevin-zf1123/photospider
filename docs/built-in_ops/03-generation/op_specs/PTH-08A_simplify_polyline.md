---
spec_schema_version: 1
id: PTH-08A
parent_id: PTH-08
function: simplify_polyline
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.simplify_polyline_v1_strict
  - path.simplify_polyline_locked_v1_strict
---

# PTH-08A: simplify polyline

Inherit [PTH-08](PTH-08_editing_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
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

## Runtime implementation

`path_refit.cpp` implements the strict ordinary and `_locked` keys. The optional locked field is UInt8 `[C]` aligned with source control rows. Farthest-anchor and split ties use the first source row. The error certificate is continuous through the arc-length correspondence. `topology=reject_unproven` currently requires a zero-error geometric-identity certificate; it fails for an unproven nonzero change rather than estimating topology from samples.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input reference O(V^2), exact algebraic length comparisons and fixed ties; not ordinary
perpendicular Douglas-Peucker.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Forward collinear simplification, reversal preservation, locked points, continuous bound
and strict small-hole proof failure.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
