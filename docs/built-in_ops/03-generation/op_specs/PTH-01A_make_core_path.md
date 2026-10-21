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

Ordered verbs, control_offsets, Float64 controls[C,2], subpath_offsets, UInt8 closed, Int64 counts[3] as six explicit input tensors; CoreVerbs PathSet follows the six-member photospider.ops.path_core v1 directory in the shared geometry contract.

## Parameters and domain

pixel-xy-right-down coordinates; schema segment/control limits. Empty paths use positive static-capacity Tensor members and ordinary payload counts equal to zero, not dynamic or zero-extent tensors. Capacities and canonical padding follow the shared Core schema; counts is an explicit input.

## Mathematical specialization

M/L/Q/C/Z arities1/1/2/3/0, valid offsets and terminal Z/closed consistency are unchecked caller premises; copy
control bits, preserve zero-length segments. Canonical empty offsets=[0]. Width and source-map Results are supplied through separate workflows; this constructor does not attach metadata.

## Dependencies, resources and failures

Computation scope: **complete input in a Run-shared stage**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, following the declared dependencies and static structure checks.

complete-input O(verbs+controls); no observable partial bundle.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural errors and operational NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

M(0,0),L(3,4), M-only path, canonical empty data, valid offsets/associations and closed paths.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
