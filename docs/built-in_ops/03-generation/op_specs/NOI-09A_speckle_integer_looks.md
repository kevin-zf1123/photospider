---
spec_schema_version: 1
id: NOI-09A
parent_id: NOI-09
function: speckle_integer_looks
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.speckle_integer_looks_v1_strict
  - noise.speckle_integer_looks_v1_accelerated_apple_silicon
  - noise.speckle_integer_looks_v1_accelerated_x86_64
---

# NOI-09A: speckle integer looks

Inherit [NOI-09](NOI-09_grain_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Runtime implementation

The registered CPU profiles above read Float32/Float64 HWC signal and publish
`values` in the requested dtype (default float64). Looks, seed and stream are
required; frame/origins default to zero. Optional height/width must match the input
schema and C<=65536. `max_refinements` defaults to 4096; `max_work` independently
bounds computation. SIMD logarithms provide outward enclosures for the complete
sum/division/product. Strict FP32 accepts only identical rounded endpoints;
accelerated FP64 follows the shared final-output bound. Other cases retain directed
certification at 128 through 4096 bits. Signed-zero signal preserves multiplication
sign. No GPU key is registered.

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
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
