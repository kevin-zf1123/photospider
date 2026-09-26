---
spec_schema_version: 1
id: PTH-01C
parent_id: PTH-01
function: split_subpaths
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.split_subpaths_v1_strict
---

# PTH-01C: split subpaths

Inherit [PTH-01](PTH-01_construction_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet and group_of_subpath[K] Int64 Result field; flat PathSet plus associated
PathPartition Result.

## Parameters and domain

Static group_count G>=1; group IDs in [0,G); preserve within-group order.

## Mathematical specialization

Stable-group subpaths into one flat Result; partition offsets have G+1 entries and
represent empty groups. No dynamic output-port count or nested Result. Remap
global-width/source associations rather than copying stale normalized coordinates.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole O(K+rows+G); charged permutation and same-snapshot associations.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

groups1,0,1 yields source order1,0,2 and offsets0,1,3; empty group and invalid ID.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
