---
spec_schema_version: 1
id: MASK-04C
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-04
function: hue_range
oracle_entry: hue_range
registered_operation_keys:
  - mask.hue_range_strict
---

# MASK-04C: Periodic hue selector with explicit neutral policy

Inherits the [family contract](MASK-04_color_range_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.hue_range_strict`.

image: Field[C,H,W] -> values: Coverage[H,W]

Static parameters: channels selects hue,chroma; optional input_description; center_turns finite; minimum_chroma>=0 finite; neutral_policy include|exclude; inner,outer in [0,.5]; curve.

## Mathematics

Use exact turn reduction. A sample is neutral when chroma is exactly zero or strictly below minimum_chroma. The explicit neutral_policy returns 1 for include and 0 for exclude; the authoring recommendation is exclude. Equality to a positive minimum_chroma uses the hue branch. Selected-coordinate domains are unchecked premises; unused polar coordinates add no reads.

Support: hue/chroma Data only; no spatial halo.

## Acceptance

hue=[0,1,2,-1], chroma=1, center=0, inner=outer=0 -> [1,1,1,1]; neutral exclusion gives 0.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
