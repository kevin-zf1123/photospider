---
spec_schema_version: 1
id: FIL-15
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- FIL-15A
- FIL-15B
- FIL-15C
research_sources:
- S07
---

# FIL-15: Gaussian/Laplacian pyramids

The binomial kernel, sampling phase, and paired expansion are fixed; approximate round-trip error is not described as lossless.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Operation spec | Composition type | Numerical reference |
| --- | --- | --- | --- |
| FIL-15A | [Gaussian pyramid](FIL-15A_gaussian_pyramid.md) | primitive | S |
| FIL-15B | [Laplacian pyramid](FIL-15B_laplacian_pyramid.md) | primitive | S |
| FIL-15C | [Laplacian pyramid reconstruction](FIL-15C_reconstruct_laplacian.md) | primitive | S |

### pyramid_binomial5_v1

The one-dimensional kernel is h=[1,4,6,4,1]/16; the 2D kernel is its exact outer product; boundary=reflect_half. Reduce first applies the exact 2D convolution and RN_t, then samples even coordinates (2y,2x), with ceil-sized output. For expand, define the coarse image on an infinite integer grid and extend it with reflect_half. At each original target coordinate q, accumulate only offsets d∈[-2,2]² for which q-d is even, using coarse coordinate (q-d)/2 and weight 4*h[dy]*h[dx], then sum exactly and round once to RN_t. This differs from cropping the target first and then extending its boundary. Both axes are scaled by two. Read target_shape from the saved original-level descriptor; do not guess parity. One expand/reduce pair is not an inverse; the Laplacian residual compensates for this rather than assuming inverse filtering.

## Numeric semantics

Numeric classes E, B and S, and NaN/Inf, signed-zero, overflow and underflow behavior, follow the [numeric reference](FILTER_numeric_reference.md) and the corresponding NUM operations. Positive-weight normalization, signed kernels, local regression and global inverse problems have distinct alpha semantics; see [color composition](FILTER_color_composition.md). Spatial extension, anchors and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collections contract](FILTER_collections_contract.md). Shapes depend only on descriptors.

## Behavior

Each member's declared support is normative. A halo does not make an arbitrarily cropped ROI an independent image boundary, and a whole-image dependency stays whole-image. Staged rounding, global connectivity, transforms and iterative state follow each member's definition.

## References

[S07 · Paris, Hasinoff, Kautz: Local Laplacian Filters](../research-sources.md#s07)
