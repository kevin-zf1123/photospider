---
spec_schema_version: 1
id: NOI-07B
parent_id: NOI-07
function: blue_threshold_points
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# NOI-07B: blue threshold points

Inherit [NOI-07](NOI-07_blue_noise_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Rank/resource as NOI-07A; DynamicPoints Result, at most one point per canvas pixel.

## Parameters and domain

canvas,offsets,threshold_count K=0..N; hard output capacity max_count.

## Mathematical specialization

Select rank<K, publish global pixel centers RN64 in y/x scan order with ordinal IDs. One
complete resource period yields exactly K points; cropped/repeated coverage changes
count. K=0 is empty. No continuous Poisson-disk or minimum-distance claim.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole count/allocate/fill, O(HW+N); capacity exhaustion fails. This capacity is not
GEN-08C/D's normal count target.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Full-period counts, nested thresholds, crop/origin invariance and capacity rejection.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
