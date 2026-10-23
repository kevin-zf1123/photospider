---
spec_schema_version: 1
id: NOI-08B
parent_id: NOI-08
function: shot_electrons
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# NOI-08B: shot electrons

Inherit [NOI-08](NOI-08_poisson_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Runtime implementation

The public `ps::generation::shot_electrons` helper in
`photospider/ops/generation.hpp` appends the NOI-08A strict node and returns its
`counts` output reference. It takes the expected-electron input reference and
explicit NOI-08A parameters. Electron units and upstream calibration remain
caller-owned; no extra conversion, rounding stage or random source is added.

## Interface and representation

expected_electrons[H,W,C] finite nonnegative; electrons same shape Int64.

## Parameters and domain

NOI-08A parameters; nonnegative expected electrons are an unchecked caller premise; explicit upstream linear-light/electron
calibration.

## Mathematical specialization

Compose NOI-08A with electron units. No implicit dark current, read noise, full well,
gain, black level, clipping or ADC. Finite random-grid approximation is disclosed
separately from ideal Poisson mean/variance.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional count generation; retain explicit conversion stages and complete parameter
identity.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Zero exposure, nonnegative expected-electron inputs, unit binding; no EMVA/camera calibration
claim.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
