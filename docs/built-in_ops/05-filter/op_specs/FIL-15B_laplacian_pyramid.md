---
spec_schema_version: 1
id: FIL-15B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-15
function: laplacian_pyramid
proposed_operation_keys:
- filter.laplacian_pyramid_strict
- filter.laplacian_pyramid_accelerated_apple_silicon
- filter.laplacian_pyramid_accelerated_x86_64
numeric_reference: S
oracle_scope: mathematical_reference
research_sources:
- S07
---

# FIL-15B: laplacian_pyramid

Laplacian pyramid.

Inherits [FIL-15](FIL-15_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.laplacian_pyramid_strict`, `filter.laplacian_pyramid_accelerated_apple_silicon`, `filter.laplacian_pyramid_accelerated_x86_64`.

`input` → FilterBands/v1 `bands`, with roles matching the family algorithm. The native members are `detail_0` through `detail_(L-1)` followed by `base`, in schema `photospider.filter_bands.pyramid_binomial5_v1` version 1. A native Result holds at most 16 members (0..15 reductions); the SDK helpers `ps::filter::gaussian_pyramid`, `laplacian_pyramid` and `reconstruct_laplacian` (declared in `photospider/ops/filter.hpp`, with `PyramidOptions`, `PyramidBand` and `PyramidBands`) split deeper pyramids into blocks of at most 15 reductions, joined through the stored base without an extra rounding stage. The logical FilterBands/v1 directory records IDs, source grid, original and level shapes, dtype, level/role, valid crop, even-phase sampling step, origin/spacing/unit, analysis/synthesis profile, rounding profile and padding history. Optional `expected_shape` and `expected_dtype` parameters bind descriptors to the SDK directory.

### Parameters

Axes are explicit; levels L≥0. Do not add levels after repeated reduction reaches H=W=1 (L is at most the number of levels needed to reach 1×1). The profile is fixed to pyramid_binomial5_v1; copy all non-spatial axes.

## Mathematics

Numerical class: **S**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Generate G using FIL-15A. Compute each L_l by an exact pointwise difference followed by RN_t: `L_l=RN_t(G_l-expand(G(l+1)))`. Publish L0..L(L-1) and base=GL; do not publish intermediate G levels.

## Behavior

The static band directory records roles, dtype, shape and phase. A requested band depends on its mathematical ancestors; a deeper low-frequency request can require preceding low-frequency stages but not unrelated sibling bands. At L=0 level 0 is an identity copy of the input.

## Acceptance

Constant inputs produce identical G levels and +0 details. For odd 3×5 dimensions, preserve each level shape. L=0 identity preserves signed zero. Editing one detail changes only its corresponding reconstruction scale. Numerical-error checks must not require arbitrary floats to round-trip losslessly.

## References

[S07 · Paris, Hasinoff, Kautz: Local Laplacian Filters](../research-sources.md#s07)
