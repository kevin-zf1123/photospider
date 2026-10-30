---
spec_schema_version: 1
id: FIL-03
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- FIL-03A
- FIL-03B
research_sources:
- S01
---
# FIL-03: Box statistics

Box sum and box mean are separate operators; neither is a resize operation.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Member | Kind | Numeric reference |
| --- | --- | --- | --- |
| FIL-03A | [Rectangular sum](FIL-03A_box_sum.md) | primitive | E |
| FIL-03B | [Rectangular mean](FIL-03B_box_mean.md) | primitive | E |

## Numeric semantics

Numeric classes E, B and S, and NaN/Inf, signed-zero, overflow and underflow behavior, follow the [numeric reference](FILTER_numeric_reference.md) and the corresponding NUM operations. Positive-weight normalization, signed kernels, local regression and global inverse problems have distinct alpha semantics; see [color composition](FILTER_color_composition.md). Spatial extension, anchors and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collections contract](FILTER_collections_contract.md). Shapes depend only on descriptors.

## Behavior

Each member's declared support is normative. A halo does not make an arbitrarily cropped ROI an independent image boundary, and a whole-image dependency stays whole-image. Staged rounding, global connectivity, transforms and iterative state follow each member's definition.

## References

[OpenCV Image Filtering](../research-sources.md#s01).
