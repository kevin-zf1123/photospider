---
spec_schema_version: 1
id: MASK-17D
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-17
function: apply_alpha
oracle_entry: apply_alpha
registered_operation_keys:
  - mask.apply_alpha_strict
---

# MASK-17D: Apply coverage to internal straight-image alpha only

Inherits the [family contract](MASK-17_apply_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.apply_alpha_strict`.

image: Float32/64[C,H,W], mask: same-dtype Coverage[H,W] -> values: image dtype/shape

Static parameters: alpha_channel Int64 matching the required input_description.

## Mathematics

Alpha=RN_T(a*m), mask1 copies a; all straight colors/AOV are exact bypass.

Support: alpha output reads alpha+mask; color/AOV outputs only their own input samples.

## Acceptance

straight RGBA=(.8,.2,.1,.5), mask=.25 -> (.8,.2,.1,.125), not (.2,.05,.025,.125).

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
