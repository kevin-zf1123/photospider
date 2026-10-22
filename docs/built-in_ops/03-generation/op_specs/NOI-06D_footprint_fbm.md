---
spec_schema_version: 1
id: NOI-06D
parent_id: NOI-06
function: footprint_fbm
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.footprint_fbm_v1_strict
  - noise.footprint_fbm_v1_accelerated_apple_silicon
  - noise.footprint_fbm_v1_accelerated_x86_64
---

# NOI-06D: footprint fbm

Inherit [NOI-06](NOI-06_fractal_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
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

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional; assume demanded coordinates/footprint and shared controls satisfy their domains; this is an
antialias heuristic, not strict band limitation.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

footprint0 matches A; footprint*frequency>=1 gives +0; continuous cutoff; spectral
quality distinct from numeric accuracy.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
