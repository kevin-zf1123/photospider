---
spec_schema_version: 1
id: NOI-02A
parent_id: NOI-02
function: gaussian_box_muller
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.gaussian_box_muller_v1_strict
  - noise.gaussian_box_muller_v1_accelerated_apple_silicon
  - noise.gaussian_box_muller_v1_accelerated_x86_64
---

# NOI-02A: gaussian box muller

Inherit [NOI-02](NOI-02_gaussian_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

mean[C], sigma[C] Float32/64; values[H,W,C].

## Parameters and domain

canvas/dtype,seed/stream/frame, domain 2; finite mean, finite sigma>=0,C<=65536.

## Mathematical specialization

sigma=0 returns RN_T(mean) without log. Otherwise open52 u and halfopen53 v define
RN_T(mean+sigma*sqrt(-2*ln(u))*cos(2*pi*v)). No intermediate double rounding; one
independent cos sample per channel, no neighbor pairing. For each channel, take u from word 0 and v from word 1 of the same domain 2 block, using the shared open52 and halfopen53 mappings. Words 2 and 3 are unused; draw is fixed to 0 without a user-adjustable draw parameter.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and the controls required by the mathematics. Complete control reads do not imply runtime payload validation. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output mathematics, following the declared dependencies and static structure checks.

Regional pixels; mean/sigma controls are assumed valid; certified transcendental
refinement with explicit failure.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural errors and operational NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

sigma zero, open endpoints, analytic quarter-turn phases, small sigma/tails. Measured
Box-Muller is not a strict golden.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
