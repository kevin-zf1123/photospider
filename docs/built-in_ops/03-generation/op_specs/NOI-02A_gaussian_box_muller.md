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

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional pixels; mean/sigma controls are assumed valid; certified transcendental
refinement with explicit failure.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

sigma zero, open endpoints, analytic quarter-turn phases, small sigma/tails. Measured
Box-Muller is not a strict golden.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
