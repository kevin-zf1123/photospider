---
spec_schema_version: 1
id: PTH-06B
parent_id: PTH-06
function: transform_stroke_outline
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# PTH-06B: transform stroke outline

Inherit [PTH-06](PTH-06_transforms_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Closed stroke outline PathSet and affine[2,3]; transformed closed outline.

## Parameters and domain

Nonsingular affine only; no centerline-width propagation.

## Mathematical specialization

Transform the outline geometry, then fill explicitly using PTH-09. A round cap becomes
elliptical under nonuniform scaling; do not scale a raster mask instead. Propagate prior
polygonization error by at most spectral norm(A) and account subsequent publication
error.

## Runtime implementation

`generation::transform_stroke_outline` is a public authoring helper. It composes PTH-06A with `path.transform_outline_report_v1_strict`; its explicit inputs include the prior GeometryReport. The report kernel computes the affine spectral norm with certified radicals, scales prior stage bounds, and adds construction-time coordinate publication error. Filling the transformed closed outline is a separate PTH-09 call.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input transformed geometry; downstream fill Regional.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Width2 horizontal outline scaled3 in y has height6, unlike centerline-transform then
width2 stroke; reflected orientation.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
