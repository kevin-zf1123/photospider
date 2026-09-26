---
spec_schema_version: 1
id: NOI-04B
parent_id: NOI-04
function: gradient2d_philox
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.gradient2d_philox_v1_strict
  - noise.gradient2d_philox_v1_accelerated_apple_silicon
  - noise.gradient2d_philox_v1_accelerated_x86_64
---

# NOI-04B: gradient2d philox

Inherit [NOI-04](NOI-04_gradient_noise_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

coordinates[S...,2]; values[S...] Float32/64.

## Parameters and domain

dtype,seed/stream/frame; corner coordinates signed32; period_x/y=0 or 1..2^31-1.

## Mathematical specialization

Exact floor/local fraction; positive periods apply Euclidean modulo to valid corner
indices before RNG. Domain6, channel/draw=0; low three selected word bits choose
(1,0),(-1,0),(0,1),(0,-1),(1,1),(-1,1),(1,-1),(-1,-1). Diagonals are not normalized.
Four fade5-weighted dots form one RN expression.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

O(4*count(Q)); charged gradient cache optional; RNG mapping gate.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Integer zeros, C2 seams, explicit periods, period1, address bounds and exact gradient
selection. period0 does not promise mathematical aperiodicity.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
