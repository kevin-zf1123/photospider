---
spec_schema_version: 1
id: PTH-04C
parent_id: PTH-04
function: resample_arc_spacing
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.resample_arc_spacing_v1_strict
---

# PTH-04C: resample arc spacing

Inherit [PTH-04](PTH-04_resampling_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet,positive spacing[1] in px; PathSamplesArc Result.

## Parameters and domain

include_open_end default true, hard max_count; closed seam not repeated.

## Mathematical specialization

Per subpath emit0,delta,2delta,... strictly below true L; optionally append open L once.
L0 emits one valid point. Determine count from certified true length, not its
already-rounded published approximation. Invert as PTH-04B.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole; certify count before allocation, refine borderline length comparisons.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

L100/delta30 emits0,30,60,90,100 when open; closed excludes100; exact multiples and
near-boundary lengths.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
