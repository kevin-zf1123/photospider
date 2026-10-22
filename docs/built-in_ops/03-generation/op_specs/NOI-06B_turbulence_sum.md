---
spec_schema_version: 1
id: NOI-06B
parent_id: NOI-06
function: turbulence_sum
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.turbulence_sum_v1_strict
  - noise.turbulence_sum_v1_accelerated_apple_silicon
  - noise.turbulence_sum_v1_accelerated_x86_64
---

# NOI-06B: turbulence sum

Inherit [NOI-06](NOI-06_fractal_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Same ports as NOI-06A.

## Parameters and domain

Same parameters and unchecked premises as NOI-06A.

## Mathematical specialization

Use NOI-06A's exact octave construction with term_i=abs(n_i), one final RN. Preserve
shared-scale and zero-amplitude rules.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Same dependency and resource rules as NOI-06A.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Nonnegative output, integer origin zero, sign-invariant term.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
