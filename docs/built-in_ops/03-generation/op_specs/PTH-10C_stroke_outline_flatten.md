---
spec_schema_version: 1
id: PTH-10C
parent_id: PTH-10
function: stroke_outline_flatten
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.stroke_outline_flatten_v1_strict
---

# PTH-10C: stroke outline flatten

Inherit [PTH-10](PTH-10_stroke_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Explicit constant or variable-round stroke recipe; closed CoreVerbs outline and
GeometryReport.

## Parameters and domain

epsilon_outline_px>0 default0.05; max_segments,attribute_policy; topology allow_change
default or reject_unproven.

## Mathematical specialization

Construct the exact exposed stroke boundary then deterministic equal-angle DFS
subdivision of arcs, bounding chord sagitta plus RN64 endpoint displacement. Straight
edges preserved, contours canonicalized. Default reports unverified topology; strict
policy proves required feature/face relations or fails. Output polygon area is not
original curved-stroke area.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole geometry Result; arrangement and output work bounded, no implicit width
modification or unreported quality degradation.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Butt line rectangle, circle outline bound, narrow-hole strict failure and permitted
geometric difference from direct exact coverage.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
