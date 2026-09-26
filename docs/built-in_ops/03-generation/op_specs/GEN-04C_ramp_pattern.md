---
spec_schema_version: 1
id: GEN-04C
parent_id: GEN-04
function: ramp_pattern
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.ramp_pattern_v1_strict
  - generation.ramp_pattern_v1_accelerated_apple_silicon
  - generation.ramp_pattern_v1_accelerated_x86_64
---

# GEN-04C: ramp pattern

Inherit [GEN-04](GEN-04_test_patterns_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

No Value inputs; values[H,W].

## Parameters and domain

canvas, axis=x/y, dtype default Float64; finite lo/hi default 0/1.

## Mathematical specialization

For axis length N>1 return RN_T(((N-1-i)*lo+i*hi)/(N-1)), copying selected endpoints.
N=1 returns lo. This differs from edge-normalized pixel centers.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

O(area(Q)).

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

N=3 gives 0,0.5,1; N=1 gives lo; descending endpoints allowed.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
