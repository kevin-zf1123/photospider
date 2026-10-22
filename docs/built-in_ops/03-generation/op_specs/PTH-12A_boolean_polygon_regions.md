---
spec_schema_version: 1
id: PTH-12A
parent_id: PTH-12
function: boolean_polygon_regions
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# PTH-12A: boolean polygon regions

Inherit [PTH-12](PTH-12_boolean_offset_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Two Polyline PathSets; Float64 CoreVerbs result and associated GeometryReport.

The associated Result uses `photospider.ops.geometry_report` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

## Parameters and domain

union/intersection/difference/xor; independent nonzero/evenodd fill rules;
epsilon_publication_px>0 candidate default1e-9; hard max_events; attribute_policy
default reject_unmappable.

## Mathematical specialization

Lift input floats exactly, perform rational regularized Boolean, then publish using
PTH-12E; equivalent to PTH-12C followed by E. Canonicalize outer shoelace positive in
y-down, holes negative; remove shape-neutral degree2 collinear vertices, rotate each
contour to lexicographically minimal full cycle, sort cycles. RN64 coordinates once,
canonical +0. Displacement within the requested budget, preserved incidence and absence
of new crossings or merges are unchecked caller premises. Accumulate displacement
while constructing coordinates and report unverified topology; no publication scan.
No snapping or
silent tiny-hole removal.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input Result; typical arrangement O((E+I)log E), O(E^2) reference; all
events/limbs/publication work charged.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Rectangle Boolean areas, contacts/holes, rational intersections, representable small
features, idempotence and xor self-empty. Publication fixtures satisfy the unchecked
rounding/topology premises; invalid geometry does not require a runtime diagnostic.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
