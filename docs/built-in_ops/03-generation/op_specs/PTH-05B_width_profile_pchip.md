---
spec_schema_version: 1
id: PTH-05B
parent_id: PTH-05
function: width_profile_pchip
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# PTH-05B: width profile pchip

Inherit [PTH-05](PTH-05_width_profiles_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Ports and width domains as PTH-05A; values[N].

## Parameters and domain

Same unchecked domain premises; PCHIP slopes and short-table behavior inherit CRV-01B.

## Mathematical specialization

Explicitly publish NUM-computed u=s/L or s (zero length0), then evaluate CRV-01B.
Preserve this intermediate RN difference from A's direct expression. Shape preservation
needs no post-hoc max(0,w).

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional query target with actual composed producer demands; full controls and slope
scratch charged.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Nonmonotone nonnegative widths, endpoint hits, short-table linear reduction and per-node
rounding agreement.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
