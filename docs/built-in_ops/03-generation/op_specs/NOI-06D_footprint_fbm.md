---
spec_schema_version: 1
id: NOI-06D
parent_id: NOI-06
function: footprint_fbm
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.footprint_fbm_v1_strict
  - noise.footprint_fbm_v1_accelerated_apple_silicon
  - noise.footprint_fbm_v1_accelerated_x86_64
---

# NOI-06D: footprint fbm

Inherit [NOI-06](NOI-06_fractal_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

coordinates[S...,D], footprint[S...] finite nonnegative pre-frequency coordinate radius;
values[S...].

## Parameters and domain

NOI-06A parameters; fbm terms only; explicit upstream footprint, never inferred from ROI
size.

## Mathematical specialization

Inherit NOI-06A's exact basis coordinates, full weighted sum and one final RN, then
multiply a_i by clamp(2-2*footprint*frequency*lacunarity^i,0,1). amplitude_sum
denominator is the unattenuated sum gain^i, avoiding cutoff division by zero or
amplification. Proven zero weights may skip basis evaluation.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional; validate demanded coordinates/footprint and shared controls; this is an
antialias heuristic, not strict band limitation.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

footprint0 matches A; footprint*frequency>=1 gives +0; continuous cutoff; spectral
quality distinct from numeric accuracy.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
