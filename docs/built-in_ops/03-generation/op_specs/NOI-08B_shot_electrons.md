---
spec_schema_version: 1
id: NOI-08B
parent_id: NOI-08
function: shot_electrons
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# NOI-08B: shot electrons

Inherit [NOI-08](NOI-08_poisson_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

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

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and the controls required by the mathematics. Complete control reads do not imply runtime payload validation. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output mathematics, following the declared dependencies and static structure checks.

Regional count generation; retain explicit conversion stages and complete parameter
identity.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural errors and operational NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Zero exposure, nonnegative expected-electron inputs, unit binding; no EMVA/camera calibration
claim.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
