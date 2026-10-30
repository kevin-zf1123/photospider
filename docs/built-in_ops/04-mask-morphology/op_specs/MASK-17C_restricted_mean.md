---
spec_schema_version: 1
id: MASK-17C
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-17
function: restricted_mean
oracle_entry: restricted_mean
registered_operation_keys:
  - mask.restricted_mean_strict
---

# MASK-17C: Mask-restricted local mean with denominator validity

Inherits the [family contract](MASK-17_apply_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.restricted_mean_strict`.

image: Float32/64[C,H,W], mask: same-dtype Coverage[H,W] -> values: Float32/64[C,H,W], valid: Binary[H,W]

Static parameters: MASK-05 footprint schema; empty_policy zero|error.

## Mathematics

Exact all-sample weighted numerator and weight-sum denominator; explicit zero-denominator behavior on the finite input domain.

Support: mask support S; all image samples in S for requested value channels, including zero weights; valid alone has no image Data.

Binary flags use the associated value output floating dtype and exact 0/1.

## Acceptance

image=[0,10], weights=[1,0] and a footprint covering both ->0, not5; weights[1,1] ->5; allzero ->valid0.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
