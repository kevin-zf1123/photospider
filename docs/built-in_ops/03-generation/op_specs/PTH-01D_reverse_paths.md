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

Open paths reverse segments/start point. Closed CoreVerbs paths retain the original M anchor, reverse the cycle and represent closure once with Z. Primitive paths use their own reference representation rather than M/Z. The v1 reverse domain requires a representable reverse within the same static pools; closed Primitive subpaths containing a terminal Close reference are outside that domain. In particular, an ArcSweep followed by a Close with non-Float64 endpoints cannot be reversed while retaining its start by inserting an ordinary Float64 Bezier line. This is an unchecked caller premise, not a new runtime admission scan or a promise to convert representations. Closed Primitive loops represented entirely by ordinary curve references, including FullTurn, retain their start as representability permits. Reverse Bezier controls; Hermite
becomes(P1,P0,-d1,-d0); negate arc direction; reverse spline controls/weights and map
knots k_i to a+b-k_(m-i). Mirror arc bindings within each original subpath length
interval, not whole-PathSet u->1-u.

## Dependencies, resources and failures

Computation scope: **complete input in a Run-shared stage**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, following the declared dependencies and static structure checks.

complete-input O(rows) plus binding remap; RN64 for new arc/spline values, assume representable output connections
without epsilon snapping.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural errors and operational NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Reverse twice on the supported domain, fixed closed Core anchor, 3-4-5 line, multisubpath width mapping and signed
derivative zeros.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
