---
spec_schema_version: 1
id: NOI-08A
parent_id: NOI-08
function: poisson_icdf
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.poisson_icdf_v1_strict
---

# NOI-08A: poisson icdf

Inherits [NOI-08](NOI-08_poisson_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.poisson_icdf_v1_strict`.

lambda[H,W,C] finite nonnegative; counts same shape Int64.

### Parameters and domain

canvas matches lambda; dtype of lambda Float32/64; domain8 RNG; lambda maximum1,000,000, max_count_search default2,000,000; C<=65536.

Input: Float32/Float64 HWC `lambda`; output: Int64 `counts`. Parameters: Int64 `seed`, `stream` (required); Int64 `frame` and origins (default 0); optional `height`/`width` must match the input schema; Int64 `max_count_search` (default 2,000,000); Int64 `max_refinements` (default 4096). The admitted lambda domain is 0..1,000,000. If the count search or its certification is exhausted, the operation fails with `OperationFailed`/NotConverged domain code `0x4e4f490002`.

## Mathematics

Use global pixel x/y, logical channel and caller seed/stream/frame with domain 8 and draw fixed to 0. Word 0 supplies open52; other words are unused. lambda=0 yields0. Otherwise take open52 u and smallest k>=0 with exp(-lambda)*sum_j=0..k lambda^j/j! >=u; equality selects k. Compare with certified bounds; ordinary double CDF saturation is not proof. The finite-grid quantile is not the ideal continuous-source PMF.

## Behavior

Support class: Regional.

## Acceptance

Zero rate, lambda1/u0.5 gives1, adjacent CDF thresholds, large-rate underflow avoidance and exact integer decisions.
