---
spec_schema_version: 1
id: GEN-04D
parent_id: GEN-04
function: impulse_pattern
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.impulse_pattern_v1_strict
---

# GEN-04D: impulse pattern

Inherit [GEN-04](GEN-04_test_patterns_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

index[2] Int64 in local xy order; values[H,W].

## Parameters and domain

canvas, dtype default Float64; finite amplitude/background default 1/0; index must be in
bounds.

## Mathematical specialization

Select amplitude exactly at index, background elsewhere. This is a discrete pixel
impulse, not a continuous Dirac distribution or subpixel splat.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and the controls required by the mathematics. Complete control reads do not imply runtime payload validation. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output mathematics, following the declared dependencies and static structure checks.

O(area(Q)); the index is in bounds as a caller premise, including zero amplitude.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural errors and operational NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

3x3 center impulse; valid boundary indices; zero amplitude.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
