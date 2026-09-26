---
spec_schema_version: 1
id: PTH-04B
parent_id: PTH-04
function: resample_uniform_arc
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.resample_uniform_arc_v1_strict
---

# PTH-04B: resample uniform arc

Inherit [PTH-04](PTH-04_resampling_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet; PathSamplesArc Result with positions,source_segment,local_t,arc_s.

## Parameters and domain

count_per_subpath n>=1; hard max_count; open n1 takes start; closed endpoints not
repeated.

## Mathematical specialization

Open n>=2 targets s_j=jL/(n-1); closed uses jL/n. L0 repeats the valid start n times.
Invert true length; interior positive-length junction chooses following segment t0,
final open endpoint last valid segment t1. Position RN64(B(t*)) and local_t RN64(t*) are
independent; do not reconstruct position using rounded t.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole; certified inverse using table enclosures, not linear interpolation of an
approximate table.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

100px line five samples, closed400px square four corners, zero length and no
inter-subpath jump length.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
