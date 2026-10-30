---
spec_schema_version: 1
id: FIL-13
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- FIL-13A
- FIL-13B
research_sources:
- S06
---

# FIL-13: Canny and hysteresis connectivity

Spatial filtering is not global connectivity; final edges require coordination across tiles.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Operation spec | Composition type | Numerical reference |
| --- | --- | --- | --- |
| FIL-13A | [Static Canny workflow](FIL-13A_canny_quantized4.md) | authoring_helper | S |
| FIL-13B | [Weak/strong edge connectivity](FIL-13B_hysteresis_edges.md) | primitive | discrete exact |

## Shared mathematics, units, and profiles

This draft uses quantized four-direction `quantized4_v1` NMS and is not bit-compatible with libraries that interpolate NMS directions. The low threshold must be strictly greater than zero so an all-zero response does not become weak; comparisons include endpoints.

## Numeric semantics

Numeric classes E, B and S, and NaN/Inf, signed-zero, overflow and underflow behavior, follow the [numeric reference](FILTER_numeric_reference.md) and the corresponding NUM operations. Positive-weight normalization, signed kernels, local regression and global inverse problems have distinct alpha semantics; see [color composition](FILTER_color_composition.md). Spatial extension, anchors and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collections contract](FILTER_collections_contract.md). Shapes depend only on descriptors.

## Behavior

Each member's declared support is normative. A halo does not make an arbitrarily cropped ROI an independent image boundary, and a whole-image dependency stays whole-image. Staged rounding, global connectivity, transforms and iterative state follow each member's definition.

## References

[S06 · scikit-image feature](../research-sources.md#s06)
