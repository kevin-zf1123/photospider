---
spec_schema_version: 1
id: MASK-17A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-17
function: affect_result
oracle_entry: affect_result
registered_operation_keys:
  - mask.affect_result_strict
---

# MASK-17A: Interpolate a processed result by a mask

Inherits the [family contract](MASK-17_apply_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.affect_result_strict`.

original,processed: Float32/64[C,H,W], mask: same-dtype Coverage[H,W] -> values: image dtype/shape

Static parameters: channels: ordered unique canonical String; no implicit alpha/color conversion.

## Mathematics

Selected channels always use RN_T((1-m)*a+m*b) under finite-operand premises, including m=0/1; unselected channels copy original.

Support: both selected operands and mask at Q, independent of weight.

## Acceptance

original=2,processed=10,m=.25 ->4; finite operands at m=0/1 preserve the stated expression.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
