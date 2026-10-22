---
spec_schema_version: 1
id: GEN-06B
parent_id: GEN-06
function: lookup_numeric_gradient
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.lookup_numeric_gradient_v1_strict
  - generation.lookup_numeric_gradient_v1_accelerated_apple_silicon
  - generation.lookup_numeric_gradient_v1_accelerated_x86_64
---

# GEN-06B: lookup numeric gradient

Inherit [GEN-06](GEN-06_gradient_lookup_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
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

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional queries, table validity is an unchecked premise; O(C*area(Q)).

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Two-entry midpoint, signed-zero endpoint copy, C=4 remains raw numeric.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
