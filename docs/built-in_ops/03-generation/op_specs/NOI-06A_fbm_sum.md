---
spec_schema_version: 1
id: NOI-06A
parent_id: NOI-06
function: fbm_sum
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.fbm_sum_v1_strict
  - noise.fbm_sum_v1_accelerated_apple_silicon
  - noise.fbm_sum_v1_accelerated_x86_64
---

# NOI-06A: fbm sum

Inherit [NOI-06](NOI-06_fractal_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

coordinates[S...,D], D=2 or3 according to basis; values[S...].

## Parameters and domain

basis=gradient2d_philox/perlin2002_3d; octaves=1..32,frequency>0,lacunarity>1,gain in
[0,1]; normalization=none/amplitude_sum default latter; basis RNG/period rules.

## Mathematical specialization

a_i=gain^i, q_i=frequency*lacunarity^i*q exactly; n_i is the unrounded mathematical
basis value. Return RN_T(sum a_i*n_i / den), den=1 or sum a_i. Skip proven
zero-amplitude terms without inventing out-of-range coordinates. Octaves share the
scaled base field, not an independence claim.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional, O(count(Q)*octaves*corners); one final error budget.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

One octave equals basis; gain0 eliminates other octaves; no per-octave four-ULP
allocation.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
