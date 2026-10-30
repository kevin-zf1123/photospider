---
spec_schema_version: 1
id: MASK-03D
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-03
function: soft_range
oracle_entry: soft_range
registered_operation_keys:
  - mask.soft_range_strict
---

# MASK-03D: Soft interval selector

Inherits the [family contract](MASK-03_threshold_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.soft_range_strict`.

input: Field[H,W] -> values: Coverage[H,W]

Static parameters: lower<=upper finite Float64; lower_width,upper_width>=0 finite Float64; curve linear|smoothstep.

## Mathematics

Use the exact family min of the lower and upper edge responses before one final RN_T.

Support: input[Q].

## Acceptance

lower=0,upper=1,widths=1,linear: x=[-.5,0,.5,1,1.5] -> [0,.5,1,.5,0].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
