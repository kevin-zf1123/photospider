---
spec_schema_version: 1
id: FIL-19A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-19
function: directional_post_aa_v1
proposed_operation_keys:
- filter.directional_post_aa_v1_strict
- filter.directional_post_aa_v1_accelerated_apple_silicon
- filter.directional_post_aa_v1_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S08
---

# FIL-19A: directional_post_aa_v1

Bounded-direction post-process AA.

Inherits [FIL-19](FIL-19_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.directional_post_aa_v1_strict`, `filter.directional_post_aa_v1_accelerated_apple_silicon`, `filter.directional_post_aa_v1_accelerated_x86_64`.

`input,edge_guide` → `output`; guide is single-plane, input components are independent, and H/W/batch dimensions match.

### Parameters

Required Int64 `y_axis`, `x_axis`, and `component_axis` use the complete sample-shape axis convention. Spatial axes are distinct; component_axis is -1 or a distinct nonspatial axis. The guide matches input dtype and shape with component extent 1. Required Float64 `threshold` is finite and >0; required Float64 `blend` is finite and in [0,1/2]. These numerical premises are unchecked. Clamp is the fixed boundary; there is no boundary parameter.

## Mathematics

Numerical class: **E**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Use the guide center and N/S/E/W neighbors; compute ex=abs(E-W), ey=abs(S-N) exactly. If max(ex,ey)≤threshold or blend=0, bit-copy the input center. Otherwise choose N/S when ex≥ey (along the approximate edge direction), else E/W; compute `RN_t((1-blend)*Icenter+blend*(Iu+Iv)/2)`. For a NaN control result, evaluate the written subtraction and abs nodes first, then propagate the leftmost NaN of `max(ex,ey)`; abs clears its sign. A generated NaN in ex therefore precedes an input-derived NaN in ey. Equality at the threshold does not smooth; directional ties choose N/S. This heuristic is project-defined and is not called SMAA/FXAA.

## Behavior

The output depends on the guide cross; an activated point uses only the selected two input neighbors and the center. At blend=0 the guide does not participate (explicit bypass). The halo is 1; the direction selection is exact.

## Acceptance

A constant guide is unchanged; a vertical step selects the along-edge direction; blend=0 preserves poisoned unread guide data; bright-line textures require human quality review.

## References

[S08 · Jimenez et al.: SMAA](../research-sources.md#s08)
