---
spec_schema_version: 1
id: PTH-08C
parent_id: PTH-08
function: smooth_chaikin
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.smooth_chaikin_v1_strict
---

# PTH-08C: smooth chaikin

Inherit [PTH-08](PTH-08_editing_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
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

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole O(2^iterations*V), all intermediate backing charged; no cross-round RN fusion.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

0->4 first round0,1,3,4; closed4 vertices become8; identity bits at zero rounds.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
