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

Use global pixel x/y, logical channel and caller seed/stream/frame with domain 8 and draw fixed to 0. Word 0 supplies open52; other words are unused. lambda=0 yields0. Otherwise take open52 u and smallest k>=0 with exp(-lambda)*sum_j=0..k
lambda^j/j! >=u; equality selects k. Compare with certified bounds; ordinary double CDF
saturation is not proof. The finite-grid quantile is not the ideal continuous-source
PMF.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional lambda reads; certified large-rate CDF/bisection; search limit without proof
gives NotConverged.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Zero rate, lambda1/u0.5 gives1, adjacent CDF thresholds, large-rate underflow avoidance
and exact integer decisions.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
