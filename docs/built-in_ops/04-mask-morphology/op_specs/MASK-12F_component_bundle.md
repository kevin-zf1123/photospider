---
spec_schema_version: 1
id: MASK-12F
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-12
function: component_bundle
oracle_entry: component_bundle
registered_operation_keys:
  - mask.component_bundle_strict
---

# MASK-12F: MinPixel labels with complete component attributes

Inherits the [family contract](MASK-12_components_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.component_bundle_strict`.

input: Binary[H,W] -> components: complete Result labels:Int64[H,W], rows:Int64[N,7] plus count:Int64[1], N=H*W

Static parameters: connectivity 4|8; maximum_count in [0,N]. Optional `max_work` as in MASK-common.

## Mathematics

B labels plus exact area,min,bbox columns as family; labels and table share one source identity.

complete-input input; one fixed multi-member Result with count, rows and labels.

## Acceptance

single foreground at row-major index3 in 2x2 -> id4,area1,min3,bbox[1,1,2,2].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
