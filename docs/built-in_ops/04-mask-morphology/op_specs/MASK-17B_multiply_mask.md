---
spec_schema_version: 1
id: MASK-17B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-17
function: multiply_mask
oracle_entry: multiply_mask
registered_operation_keys:
  - mask.multiply_mask_strict
---

# MASK-17B: Gate or scale selected numeric channels

Inherits the [family contract](MASK-17_apply_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.multiply_mask_strict`.

image: Float32/64[C,H,W], mask: same-dtype Coverage[H,W] -> values: image dtype/shape

Static parameters: channels: ordered unique canonical String.

## Mathematics

Always RN_T(m*x), with finite image values as a caller premise including m=0/1; no endpoint copy or runtime domain scan.

Support: selected mask and image at every weight; bypass channels only image.

## Acceptance

image=8,m=.25 ->2; finite image values at m=0/1 obey the formula.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
