---
spec_schema_version: 1
id: GEN-06B
parent_id: GEN-06
function: lookup_numeric_gradient
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.lookup_numeric_gradient_v1_strict
  - generation.lookup_numeric_gradient_v1_accelerated_apple_silicon
  - generation.lookup_numeric_gradient_v1_accelerated_x86_64
---

# GEN-06B: lookup numeric gradient

Inherit [GEN-06](GEN-06_gradient_lookup_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

t with shape S and table[N,C], N>=2,C>=1; values S+[C], rank<=8.

## Parameters and domain

dtype default Float64; t in [0,1]; explicit upstream spread; raw numeric table.

## Mathematical specialization

x=t*(N-1) exactly. Integer x selects that row with copy semantics; last row never
accesses N. Otherwise j=floor(x), w=x-j, return RN_T((1-w)*table[j]+w*table[j+1]). No
gamma or alpha handling.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional queries, full table validation; O(C*area(Q)).

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Two-entry midpoint, signed-zero endpoint copy, C=4 remains raw numeric.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
