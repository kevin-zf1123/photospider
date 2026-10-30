---
spec_schema_version: 1
id: FIL-04
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- FIL-04A
- FIL-04B
research_sources:
- S03
- S25
---
# FIL-04: Finite discrete Gaussian

Fixed baked64 coefficient profile. Coefficient generation and kernel application are separate operations.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Member | Kind | Numeric reference |
| --- | --- | --- | --- |
| FIL-04A | [Generate a 1D Gaussian kernel](FIL-04A_gaussian_coefficients.md) | primitive | B |
| FIL-04B | [Apply baked64 Gaussian](FIL-04B_gaussian_filter.md) | primitive | B+E |

## Numeric semantics

Numeric classes E, B and S, and NaN/Inf, signed-zero, overflow and underflow behavior, follow the [numeric reference](FILTER_numeric_reference.md) and the corresponding NUM operations. Positive-weight normalization, signed kernels, local regression and global inverse problems have distinct alpha semantics; see [color composition](FILTER_color_composition.md). Spatial extension, anchors and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collections contract](FILTER_collections_contract.md). Shapes depend only on descriptors.

## Behavior

Each member's declared support is normative. A halo does not make an arbitrarily cropped ROI an independent image boundary, and a whole-image dependency stays whole-image. Staged rounding, global connectivity, transforms and iterative state follow each member's definition.

## References

[SciPy gaussian_filter](../research-sources.md#s03); [GNU MPFR manual](../research-sources.md#s25).
