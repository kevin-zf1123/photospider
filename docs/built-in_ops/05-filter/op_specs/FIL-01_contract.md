---
spec_schema_version: 1
id: FIL-01
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- FIL-01A
- FIL-01B
- FIL-01C
- FIL-01D
research_sources:
- S01
- S02
---
# FIL-01: General 2D kernels

Direction, anchor, output coordinates, normalization, masked positive-weight convolution, and semantic color blur are separate contracts.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Member | Kind | Numeric reference |
| --- | --- | --- | --- |
| FIL-01A | [2D convolution](FIL-01A_convolve2d.md) | primitive | E |
| FIL-01B | [2D correlation](FIL-01B_correlate2d.md) | primitive | E |
| FIL-01C | [Positive-kernel convolution with participation weights](FIL-01C_normalized_convolution.md) | primitive | E |
| FIL-01D | [Positive-kernel color blur](FIL-01D_positive_color_blur.md) | primitive | E |

## Numeric semantics

Numeric classes E, B and S, and NaN/Inf, signed-zero, overflow and underflow behavior, follow the [numeric reference](FILTER_numeric_reference.md) and the corresponding NUM operations. Positive-weight normalization, signed kernels, local regression and global inverse problems have distinct alpha semantics; see [color composition](FILTER_color_composition.md). Spatial extension, anchors and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collections contract](FILTER_collections_contract.md). Shapes depend only on descriptors.

## Behavior

Each member's declared support is normative. A halo does not make an arbitrarily cropped ROI an independent image boundary, and a whole-image dependency stays whole-image. Staged rounding, global connectivity, transforms and iterative state follow each member's definition.

## References

[OpenCV Image Filtering](../research-sources.md#s01); [SciPy ndimage.convolve](../research-sources.md#s02).
