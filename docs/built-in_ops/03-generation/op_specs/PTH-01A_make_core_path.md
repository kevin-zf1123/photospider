---
spec_schema_version: 1
id: PTH-01A
parent_id: PTH-01
function: make_core_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.make_core_path_v1_strict
---

# PTH-01A: make core path

Inherit [PTH-01](PTH-01_construction_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Ordered verbs, control_offsets, Float64 controls[N,2], subpath_offsets, UInt8 closed and
optional associated attributes; CoreVerbs PathSet.

## Parameters and domain

pixel-xy-right-down coordinates; schema segment/control limits. Empty dynamic rows are
Result fields, not zero-extent Values.

## Mathematical specialization

Validate M/L/Q/C/Z arities1/1/2/3/0, offsets and terminal Z/closed consistency; copy
control bits, preserve zero-length segments. Canonical empty offsets=[0]. Rebuild all
output ObjectId associations.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole O(verbs+controls+attributes); no observable partial bundle.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

M(0,0),L(3,4), M-only path, malformed arity/offset/association, nonfinite controls and
closed mismatch.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
