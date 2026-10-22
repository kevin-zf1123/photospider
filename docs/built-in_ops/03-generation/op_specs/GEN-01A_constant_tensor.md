---
spec_schema_version: 1
id: GEN-01A
parent_id: GEN-01
function: constant_tensor
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.constant_tensor_v1_strict
  - generation.constant_tensor_v1_accelerated_apple_silicon
  - generation.constant_tensor_v1_accelerated_x86_64
---

# GEN-01A: constant tensor

Inherit [GEN-01](GEN-01_constant_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

value[C] with C>=1; values[H,W,C] of the same dtype, including C=1.

## Parameters and domain

canvas; UInt8/UInt16/Int8/Int16/Int64/Float32/Float64; no scaling.

## Mathematical specialization

Copy value[c] bit-for-bit at every requested pixel, including raw NaN payloads,
infinities and signed zero. No arithmetic or transfer conversion.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

O(C*area(Q)), bounded control window.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Signed-zero and NaN payload copies, nonzero origin, 1x1 and static rejection of mismatched component counts.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
