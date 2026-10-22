---
spec_schema_version: 1
id: GEN-06A
parent_id: GEN-06
function: spread_coordinate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.spread_coordinate_v1_strict
  - generation.spread_coordinate_v1_accelerated_apple_silicon
  - generation.spread_coordinate_v1_accelerated_x86_64
---

# GEN-06A: spread coordinate

Inherit [GEN-06](GEN-06_gradient_lookup_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Floating t of any legal complete sample_shape, including batch axes; values
retains the logical sample shape and dtype with explicit output axes.

## Parameters and domain

method=pad/repeat/reflect; finite inputs; no canvas.

## Mathematical specialization

pad clamps to [0,1] with NUM copy/signed-zero rules. repeat=t-floor(t);
reflect=1-abs(1-(t-2*floor(t/2))). Exact boundary decisions, then normal RN. Repeat
rounding to 1 stays 1; downstream lookup treats it as endpoint. Exact periodic boundary
is +0.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional pointwise demand.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

For -0.25,0,1,1.25, repeat gives 0.75,0,0,0.25 and reflect gives 0.25,0,1,0.75. Pad
preserves input -0.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
