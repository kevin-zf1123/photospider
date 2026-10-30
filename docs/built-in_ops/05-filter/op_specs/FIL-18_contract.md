---
spec_schema_version: 1
id: FIL-18
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- FIL-18A
- FIL-18B
research_sources:
- S01
---

# FIL-18: Variable-radius gather

Each output position has its own disk or box footprint; do not implement it as two passes with variable radius.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Operation spec | Composition type | Numerical reference |
| --- | --- | --- | --- |
| FIL-18A | [Variable-radius square-box mean](FIL-18A_variable_box_gather.md) | primitive | E |
| FIL-18B | [Variable-radius disk mean](FIL-18B_variable_disk_gather.md) | primitive | E |

## Numeric semantics

Numeric classes E, B and S, and NaN/Inf, signed-zero, overflow and underflow behavior, follow the [numeric reference](FILTER_numeric_reference.md) and the corresponding NUM operations. Positive-weight normalization, signed kernels, local regression and global inverse problems have distinct alpha semantics; see [color composition](FILTER_color_composition.md). Spatial extension, anchors and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collections contract](FILTER_collections_contract.md). Shapes depend only on descriptors.

## Behavior

Each member's declared support is normative. A halo does not make an arbitrarily cropped ROI an independent image boundary, and a whole-image dependency stays whole-image. Staged rounding, global connectivity, transforms and iterative state follow each member's definition.

## References

[S01 · OpenCV Image Filtering](../research-sources.md#s01)
