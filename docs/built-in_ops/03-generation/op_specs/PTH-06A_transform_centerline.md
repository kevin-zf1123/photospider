---
spec_schema_version: 1
id: PTH-06A
parent_id: PTH-06
function: transform_centerline
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.transform_centerline_v1_strict
---

# PTH-06A: transform centerline

Inherit [PTH-06](PTH-06_transforms_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet,finite affine[2,3]; same-authority transformed PathSet where representable.

## Parameters and domain

attribute_policy; width_policy=preserve_px. The caller guarantees that the selected authority can represent the transformed geometry, including any degeneration, and that required rounded coordinates and arc axes remain in its domain.

## Mathematical specialization

Control points RN64(Ap+b), derivatives/arc axes RN64(Av). Core/Bezier/Hermite can degenerate within their representation domain. Arc axes must remain representable under the caller premises; explicitly flatten an arc before applying a transform that would leave that domain. No runtime singularity scan or failure is required. Splines retain knots/weights. Identity copies bits. Non-similarity arc-domain bindings require valid
remapping or reject/drop; width numeric px value remains unchanged.

## Runtime implementation

`path_edit_basic.cpp` implements the strict member for CoreVerbs and Primitives. Points and vector controls are transformed according to their authority; identity transforms preserve the input bits. `width_policy=preserve_px` is explicit. Associated width/mapping Results remain explicit workflow inputs and are not hidden geometry attributes.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input O(controls+attributes), possible true-length remapping; representable output connections are a caller premise, without a separate connection scan.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Identity signed zeros, affine line/Bezier, representable nonuniform transforms and legal Core/Bezier/Hermite degeneration. Test arc transforms only within their representable domain; unrepresentable primitive fixtures belong to oracle input diagnostics.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
