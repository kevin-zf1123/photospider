---
spec_schema_version: 1
id: PTH-08C
parent_id: PTH-08
function: smooth_chaikin
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.smooth_chaikin_v1_strict
---

# PTH-08C: smooth chaikin

Inherit [PTH-08](PTH-08_editing_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

M/L/Z PathSet; smoothed Polyline PathSet.

## Parameters and domain

iterations0..12,max_vertices; open_endpoint_policy=preserve; internal locked corners
require explicit splitting.

## Mathematical specialization

Each edge produces Q=(3P_i+P_(i+1))/4 and R=(P_i+3P_(i+1))/4, RN64 each round before
next iteration. Preserve open endpoints; closed loops omit duplicate closure vertex.
Zero iterations copy bits. This changes shape without an epsilon or topology guarantee.

## Runtime implementation

`path_edit_basic.cpp` implements the strict member. Defaults are one iteration and `max_vertices=1048576`; each round publishes RN64 Q/R points before the next. Scratch, vertex growth and output capacities are bounded. No geometry-error or topology certificate is inferred from Chaikin smoothing.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input O(2^iterations*V), all intermediate backing charged; no cross-round RN fusion.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

0->4 first round0,1,3,4; closed4 vertices become8; identity bits at zero rounds.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
