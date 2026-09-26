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

## Parameters and domain

union/intersection/difference/xor; independent nonzero/evenodd fill rules;
epsilon_publication_px>0 candidate default1e-9; hard max_events; attribute_policy
default reject_unmappable.

## Mathematical specialization

Lift input floats exactly, perform rational regularized Boolean, then publish using
PTH-12E; equivalent to PTH-12C followed by E. Canonicalize outer shoelace positive in
y-down, holes negative; remove shape-neutral degree2 collinear vertices, rotate each
contour to lexicographically minimal full cycle, sort cycles. RN64 coordinates once,
canonical +0; verify displacement/incidence/no new crossings or merges. No snapping or
silent tiny-hole removal.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole Result; typical arrangement O((E+I)log E), O(E^2) reference; all
events/limbs/publication work charged.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Rectangle Boolean areas, contacts/holes, rational intersections, sub-ULP publication
failure, idempotence and xor self-empty.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
