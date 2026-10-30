---
spec_schema_version: 1
id: FIL-19
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- FIL-19A
- FIL-19B
research_sources:
- S08
---

# FIL-19: Post-process anti-aliasing for existing images

Static directional smoothing is separate from native SMAA 1x. Temporal AA, coverage, and minification prefilters are excluded.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Operation spec | Composition type | Numerical reference |
| --- | --- | --- | --- |
| FIL-19A | [Bounded-direction post-process AA](FIL-19A_directional_post_aa_v1.md) | primitive | E |
| FIL-19B | [Native SMAA 1x](FIL-19B_smaa_1x_native.md) | primitive | S: smaa_1x_low_luma_v1 |

## Numeric semantics

Numeric classes E, B and S, and NaN/Inf, signed-zero, overflow and underflow behavior, follow the [numeric reference](FILTER_numeric_reference.md) and the corresponding NUM operations. Positive-weight normalization, signed kernels, local regression and global inverse problems have distinct alpha semantics; see [color composition](FILTER_color_composition.md). Spatial extension, anchors and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collections contract](FILTER_collections_contract.md). Shapes depend only on descriptors.

## Behavior

Each member's declared support is normative. A halo does not make an arbitrarily cropped ROI an independent image boundary, and a whole-image dependency stays whole-image. Staged rounding, global connectivity, transforms and iterative state follow each member's definition.

## References

[S08 · Jimenez et al.: SMAA](../research-sources.md#s08)
