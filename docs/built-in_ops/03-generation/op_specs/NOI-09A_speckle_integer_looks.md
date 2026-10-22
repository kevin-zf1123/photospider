---
spec_schema_version: 1
id: NOI-09A
parent_id: NOI-09
function: speckle_integer_looks
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.speckle_integer_looks_v1_strict
  - noise.speckle_integer_looks_v1_accelerated_apple_silicon
  - noise.speckle_integer_looks_v1_accelerated_x86_64
---

# NOI-09A: speckle integer looks

Inherit [NOI-09](NOI-09_grain_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Finite raw signal[H,W,C]; same-shape Float32/64 values.

## Parameters and domain

Integer looks L=1..64; seed/stream/frame,domain9,draw j=0..L-1.

## Mathematical specialization

Each look j takes open52 from word 0 of its domain 9 block with draw=j; words 1/2/3 are unused. G=-sum_j ln(open52_j)/L; output RN_T(signal*G) with no intermediate RN of G. Ideal
continuous multiplier has mean1,variance1/L; disclose finite-grid bias. Signed raw
signals allowed; physical intensity interpretation requires nonnegative values.
Signed-zero signal follows multiplication sign.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional, O(C*area(Q)*L), certified sum/log; deterministic reduction.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Zero signal, L1 exponential case, positive output for positive signal; larger L does not
imply pixelwise smaller deviations.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
