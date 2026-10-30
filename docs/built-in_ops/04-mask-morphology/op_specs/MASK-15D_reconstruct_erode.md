---
spec_schema_version: 1
id: MASK-15D
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-15
function: reconstruct_erode
oracle_entry: reconstruct_erode
registered_operation_keys:
  - mask.reconstruct_erode_strict
---

# MASK-15D: Grayscale reconstruction by erosion

Inherits the [family contract](MASK-15_topology_reconstruction_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.reconstruct_erode_strict`.

marker,limit: same Coverage or Binary[H,W] -> values: same dtype/shape

Static parameters: connectivity 4|8.

## Mathematics

Canonicalize zero under the caller premise marker>=limit, apply synchronous family min/max iteration to exact fixed point.

complete-input both inputs; marker/limit inequalities are caller premises.

## Acceptance

marker=[0,1,1], limit=[0,.5,0] -> [0,.5,.5].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
