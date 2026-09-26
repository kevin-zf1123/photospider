---
spec_schema_version: 1
id: NOI-01A
parent_id: NOI-01
function: uniform_philox
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.uniform_philox_v1_strict
---

# NOI-01A: uniform philox

Inherit [NOI-01](NOI-01_uniform_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

No Value input; raw values[H,W,C] Float32/64.

## Parameters and domain

canvas,C=1..65536,dtype default Float64,seed/stream/frame; signed32 global coordinates;
domain 1.

## Mathematical specialization

Use Philox4x64-10 and the separately frozen address/bit mapping. Float32 returns
halfopen24 j/2^24; Float64 halfopen53 j/2^53, both exactly representable. Native Float32
is not a cast of the Float64 sequence.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional samples, O(C*area(Q)); fixed word scratch.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Official 4x64 core KATs, address bounds, negative origin, ROI/order invariance;
finite-grid statistics do not certify a runtime sequence.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
