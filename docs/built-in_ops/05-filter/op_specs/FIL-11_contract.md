---
spec_schema_version: 1
id: FIL-11
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- FIL-11A
- FIL-11B
- FIL-11C
---

# FIL-11: LoG and DoG

Sampled LoG, zero-DC-corrected LoG filtering, and a difference of two Gaussian filters are distinct mathematical objects.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function specification | Kind | Numerical reference |
| --- | --- | --- | --- |
| FIL-11A | [Sampled LoG kernel with zero-DC correction](FIL-11A_log_kernel.md) | primitive | B |
| FIL-11B | [Discrete zero-DC LoG](FIL-11B_log_filter.md) | primitive | B+E |
| FIL-11C | [Two-scale Gaussian difference](FIL-11C_difference_of_gaussians.md) | primitive | S |

## Numeric semantics

Numeric classes E, B and S, and NaN/Inf, signed-zero, overflow and underflow behavior, follow the [numeric reference](FILTER_numeric_reference.md) and the corresponding NUM operations. Positive-weight normalization, signed kernels, local regression and global inverse problems have distinct alpha semantics; see [color composition](FILTER_color_composition.md). Spatial extension, anchors and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collections contract](FILTER_collections_contract.md). Shapes depend only on descriptors.

## Behavior

Each member's declared support is normative. A halo does not make an arbitrarily cropped ROI an independent image boundary, and a whole-image dependency stays whole-image. Staged rounding, global connectivity, transforms and iterative state follow each member's definition.
