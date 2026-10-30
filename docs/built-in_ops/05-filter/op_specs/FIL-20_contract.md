---
spec_schema_version: 1
id: FIL-20
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- FIL-20A
- FIL-20B
research_sources:
- S01
- S05
---

# FIL-20: Local moments and covariance

Reuse NUM exact-moment semantics; do not clamp a negative approximate variance to zero and present it as accurate.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Operation spec | Composition type | Numerical reference |
| --- | --- | --- | --- |
| FIL-20A | [Local mean and unbiased/population variance](FIL-20A_local_mean_variance.md) | primitive | E |
| FIL-20B | [Two-field local covariance](FIL-20B_local_covariance.md) | primitive | E |

## Numeric semantics

Numeric classes E, B and S, and NaN/Inf, signed-zero, overflow and underflow behavior, follow the [numeric reference](FILTER_numeric_reference.md) and the corresponding NUM operations. Positive-weight normalization, signed kernels, local regression and global inverse problems have distinct alpha semantics; see [color composition](FILTER_color_composition.md). Spatial extension, anchors and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collections contract](FILTER_collections_contract.md). Shapes depend only on descriptors.

## Behavior

Each member's declared support is normative. A halo does not make an arbitrarily cropped ROI an independent image boundary, and a whole-image dependency stays whole-image. Staged rounding, global connectivity, transforms and iterative state follow each member's definition.

## References

[S01 · OpenCV Image Filtering](../research-sources.md#s01)；[S05 · He, Sun, Tang: Guided Image Filtering](../research-sources.md#s05)
