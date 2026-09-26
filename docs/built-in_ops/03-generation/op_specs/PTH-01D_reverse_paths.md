---
spec_schema_version: 1
id: PTH-01D
parent_id: PTH-01
function: reverse_paths
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.reverse_paths_v1_strict
---

# PTH-01D: reverse paths

Inherit [PTH-01](PTH-01_construction_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet; same-authority reversed PathSet, subpath order unchanged.

## Parameters and domain

attribute_policy; Core,Bezier,ArcSweep/FullTurn,Hermite and nonperiodic spline support
as schema permits.

## Mathematical specialization

Open paths reverse segments/start point. Closed paths retain original M anchor, reverse
the cycle and represent closure once with Z. Reverse Bezier controls; Hermite
becomes(P1,P0,-d1,-d0); negate arc direction; reverse spline controls/weights and map
knots k_i to a+b-k_(m-i). Mirror arc bindings within each original subpath length
interval, not whole-PathSet u->1-u.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole O(rows) plus binding remap; RN64 for new arc/spline values, validate connections
without epsilon snapping.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Reverse twice, fixed closed anchor, 3-4-5 line, multisubpath width mapping and signed
derivative zeros.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
