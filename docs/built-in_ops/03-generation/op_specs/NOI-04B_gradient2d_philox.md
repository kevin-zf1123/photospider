---
spec_schema_version: 1
id: NOI-04B
parent_id: NOI-04
function: gradient2d_philox
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.gradient2d_philox_v1_strict
  - noise.gradient2d_philox_v1_accelerated_apple_silicon
  - noise.gradient2d_philox_v1_accelerated_x86_64
---

# NOI-04B: gradient2d philox

Inherit [NOI-04](NOI-04_gradient_noise_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

coordinates[S...,2]; values[S...] Float32/64.

## Parameters and domain

dtype,seed/stream/frame; corner coordinates signed32; period_x/y=0 or 1..2^31-1.

## Mathematical specialization

Exact floor/local fraction; positive periods apply Euclidean modulo to valid corner
indices before RNG. Domain6, channel/draw=0; the low three bits of word 0 (word0 & 7) choose
(1,0),(-1,0),(0,1),(0,-1),(1,1),(-1,1),(1,-1),(-1,-1). Diagonals are not normalized.
Four fade5-weighted dots form one RN expression.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

O(4*count(Q)); optional gradient reuse is confined to admitted invocation-local scratch. Use the production v1 random mapping.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Integer zeros, C2 seams, explicit periods, period1, address bounds and exact gradient
selection. period0 does not promise mathematical aperiodicity.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
