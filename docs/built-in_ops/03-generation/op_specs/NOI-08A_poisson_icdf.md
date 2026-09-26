---
spec_schema_version: 1
id: NOI-08A
parent_id: NOI-08
function: poisson_icdf
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.poisson_icdf_v1_strict
---

# NOI-08A: poisson icdf

Inherit [NOI-08](NOI-08_poisson_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

lambda[H,W,C] finite nonnegative; counts same shape Int64.

## Parameters and domain

canvas matches lambda; dtype of lambda Float32/64; domain8 RNG; candidate lambda
maximum1,000,000, max_count_search2,000,000; C<=65536.

## Mathematical specialization

lambda=0 yields0. Otherwise take open52 u and smallest k>=0 with exp(-lambda)*sum_j=0..k
lambda^j/j! >=u; equality selects k. Compare with certified bounds; ordinary double CDF
saturation is not proof. The finite-grid quantile is not the ideal continuous-source
PMF.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional lambda reads; certified large-rate CDF/bisection; search limit without proof
gives NotConverged.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Zero rate, lambda1/u0.5 gives1, adjacent CDF thresholds, large-rate underflow avoidance
and exact integer decisions.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
