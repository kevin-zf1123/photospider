---
spec_schema_version: 1
id: GEN-03D
parent_id: GEN-03
function: star_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.star_path_v1_strict
---

# GEN-03D: star path

Inherit [GEN-03](GEN-03_basic_shapes_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

center[2], radii[2]=(inner,outer), rotation[1]; closed CoreVerbs PathSet.

## Parameters and domain

points n=3..4096; rotation in radians; 0<inner<=outer.

## Mathematical specialization

For k=0..2n-1 use theta=rotation+k*pi/n and alternating outer/inner radius. Publish
RN64(center+r*(cos(theta),sin(theta))) and M/L/Z connectivity. Positive rotation is
clockwise in y-down coordinates.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input Result, O(n) certified trigonometry.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

n=4 starts on positive x at rotation=0; equal radii form a regular 2n-gon; no epsilon
angle snapping.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
