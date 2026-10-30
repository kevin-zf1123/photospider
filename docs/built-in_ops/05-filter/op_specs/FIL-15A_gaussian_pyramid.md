---
spec_schema_version: 1
id: FIL-15A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-15
function: gaussian_pyramid
proposed_operation_keys:
- filter.gaussian_pyramid_strict
- filter.gaussian_pyramid_accelerated_apple_silicon
- filter.gaussian_pyramid_accelerated_x86_64
numeric_reference: S
oracle_scope: mathematical_reference
research_sources:
- S07
---

# FIL-15A: gaussian_pyramid

Gaussian pyramid.

Inherits [FIL-15](FIL-15_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.gaussian_pyramid_strict`, `filter.gaussian_pyramid_accelerated_apple_silicon`, `filter.gaussian_pyramid_accelerated_x86_64`.

`input` → FilterBands/v1 `bands`, with roles matching the family algorithm. The native members are `gaussian_0` through `gaussian_L` in schema `photospider.filter_bands.pyramid_binomial5_v1` version 1. A native Result holds at most 16 members (levels 0..15); the SDK helpers `ps::filter::gaussian_pyramid`, `laplacian_pyramid` and `reconstruct_laplacian` (declared in `photospider/ops/filter.hpp`) split deeper pyramids into blocks of at most 15 reductions without an additional rounding stage. See [FILTER collections](FILTER_collections_contract.md).

### Parameters

Axes are explicit; levels L≥0. Do not add levels after repeated reduction reaches H=W=1 (L is at most the number of levels needed to reach 1×1). The profile is fixed to pyramid_binomial5_v1; copy all non-spatial axes.

## Mathematics

Numerical class: **S**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

G0 is the input; G(l+1)=reduce(G_l). Publish G0..GL, with each level an explicit RN_t stage.

## Behavior

The static band directory records roles, dtype, shape and phase. A requested band depends on its mathematical ancestors; a deeper low-frequency request can require preceding low-frequency stages but not unrelated sibling bands. At L=0 level 0 is an identity copy of the input.

## Acceptance

Constant inputs produce identical G levels and +0 details. For odd 3×5 dimensions, preserve each level shape. L=0 identity preserves signed zero. Editing one detail changes only its corresponding reconstruction scale. Numerical-error checks must not require arbitrary floats to round-trip losslessly.

## References

[S07 · Paris, Hasinoff, Kautz: Local Laplacian Filters](../research-sources.md#s07)
