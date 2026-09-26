---
spec_schema_version: 1
id: NOI-06B
parent_id: NOI-06
function: turbulence_sum
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.turbulence_sum_v1_strict
  - noise.turbulence_sum_v1_accelerated_apple_silicon
  - noise.turbulence_sum_v1_accelerated_x86_64
---

# NOI-06B: turbulence sum

Inherit [NOI-06](NOI-06_fractal_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Same ports as NOI-06A.

## Parameters and domain

Same parameters and validation as NOI-06A.

## Mathematical specialization

Use NOI-06A's exact octave construction with term_i=abs(n_i), one final RN. Preserve
shared-scale and zero-amplitude rules.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Same dependency and resource rules as NOI-06A.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Nonnegative output, integer origin zero, sign-invariant term.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
