---
spec_schema_version: 1
id: PTH-06A
parent_id: PTH-06
function: transform_centerline
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.transform_centerline_v1_strict
---

# PTH-06A: transform centerline

Inherit [PTH-06](PTH-06_transforms_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet,finite affine[2,3]; same-authority transformed PathSet where representable.

## Parameters and domain

attribute_policy; width_policy=preserve_px; singular map allowed only when authority can
represent degeneration.

## Mathematical specialization

Control points RN64(Ap+b), derivatives/arc axes RN64(Av). Core/Bezier/Hermite can
degenerate; singular arc axes fail unless explicitly flattened first. Splines retain
knots/weights. Identity copies bits. Non-similarity arc-domain bindings require valid
remapping or reject/drop; width numeric px value remains unchanged.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole O(controls+attributes), possible true-length remapping; revalidate connections.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Identity signed zeros, affine line/Bezier, nonuniform transform and unrepresentable
primitive degeneration.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
