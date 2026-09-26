---
spec_schema_version: 1
id: GEN-04E
parent_id: GEN-04
function: zone_plate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.zone_plate_v1_strict
  - generation.zone_plate_v1_accelerated_apple_silicon
  - generation.zone_plate_v1_accelerated_x86_64
---

# GEN-04E: zone plate

Inherit [GEN-04](GEN-04_test_patterns_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

center[2], k[1], phase[1]; values[H,W].

## Parameters and domain

canvas/dtype; finite k in cycles/px^2, phase radians; finite lo/hi default 0/1.

## Mathematical specialization

RN_T(lo+(hi-lo)*(1+cos(2*pi*k*norm(p-center)^2+phase))/2). No implicit Nyquist cutoff;
aliasing is part of the diagnostic pattern.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

O(area(Q)) with certified transcendental evaluation.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

k=0, phase=0 gives hi; exact mathematical pi phase gives lo, without treating rounded
input pi as exact pi.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
