---
spec_schema_version: 1
id: MASK-08A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-08
function: gaussian_feather
oracle_entry: gaussian_feather
registered_operation_keys:
  - mask.gaussian_feather_strict
---

# MASK-08A: Finite normalized Gaussian mask feather

Inherits the [family contract](MASK-08_gaussian_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.gaussian_feather_strict`.

input: Coverage[H,W] -> values: Coverage, same dtype/shape

Static parameters: sigma_y,sigma_x; radius_y,radius_x; boundary, as family; authoring sigma=1,radius=4,zero.

## Mathematics

Evaluate the full positive weighted sum and normalization with one RN_T; certified coefficient enclosures.

Support: exact mapped rectangular support; sigma=0 has only the origin.

## Acceptance

constant 1 with replicate or reflect_half ->1; full 1x1 with zero and positive radius -> less than1; both radii0 -> bit identity.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
