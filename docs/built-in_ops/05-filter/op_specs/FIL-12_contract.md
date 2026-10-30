---
spec_schema_version: 1
id: FIL-12
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- FIL-12A
- FIL-12B
- FIL-12C
---

# FIL-12: Local Second-Order Structure

Keep the Hessian, structure tensor, and symmetric eigendecomposition distinct; a second derivative is not a first-derivative outer product.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function specification | Kind | Numerical reference |
| --- | --- | --- | --- |
| FIL-12A | [Discrete Hessian](FIL-12A_hessian2d.md) | primitive | E |
| FIL-12B | [Smoothed gradient outer-product tensor](FIL-12B_structure_tensor2d.md) | primitive | E |
| FIL-12C | [Eigenvalues and principal orientation of a 2D symmetric matrix](FIL-12C_symmetric_eigen2d.md) | primitive | E |

## Numeric semantics

Numeric classes E, B and S, and NaN/Inf, signed-zero, overflow and underflow behavior, follow the [numeric reference](FILTER_numeric_reference.md) and the corresponding NUM operations. Positive-weight normalization, signed kernels, local regression and global inverse problems have distinct alpha semantics; see [color composition](FILTER_color_composition.md). Spatial extension, anchors and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collections contract](FILTER_collections_contract.md). Shapes depend only on descriptors.

## Behavior

Each member's declared support is normative. A halo does not make an arbitrarily cropped ROI an independent image boundary, and a whole-image dependency stays whole-image. Staged rounding, global connectivity, transforms and iterative state follow each member's definition.
