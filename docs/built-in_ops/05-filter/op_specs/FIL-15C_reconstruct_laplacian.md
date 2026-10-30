---
spec_schema_version: 1
id: FIL-15C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-15
function: reconstruct_laplacian
proposed_operation_keys:
- filter.reconstruct_laplacian_strict
- filter.reconstruct_laplacian_accelerated_apple_silicon
- filter.reconstruct_laplacian_accelerated_x86_64
numeric_reference: S
oracle_scope: mathematical_reference
research_sources:
- S07
---

# FIL-15C: reconstruct_laplacian

Laplacian pyramid reconstruction.

Inherits [FIL-15](FIL-15_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.reconstruct_laplacian_strict`, `filter.reconstruct_laplacian_accelerated_apple_silicon`, `filter.reconstruct_laplacian_accelerated_x86_64`.

`bands` FilterBands/v1 → floating-point `output` with the original shape. The SDK helper is `ps::filter::reconstruct_laplacian` (declared in `photospider/ops/filter.hpp`); see [FIL-15B](FIL-15B_laplacian_pyramid.md) for the collection representation.

### Parameters

Axes are explicit; levels L≥0. Do not add levels after repeated reduction reaches H=W=1 (L is at most the number of levels needed to reach 1×1). The profile is fixed to pyramid_binomial5_v1; the inverse does not accept conflicting levels/shape parameters.

## Mathematics

Numerical class: **S**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Starting from the base, compute each level as `R_l=RN_t(L_l+expand(R(l+1),saved_shape_l))`. Any edited coefficients conforming to the schema are allowed. Subtraction/addition rounding means analysis and reconstruction of arbitrary floats are not guaranteed to reproduce the original bitwise.

## Behavior

The static band directory (complete member directory, roles, matching dtype, ceil-halved level shapes, batch and tensor layout, phase) must be compatible. Runtime association payloads are caller premises. The output depends on every detail band and the base. Reconstruction does not require the original producer identity; it accepts reordered entries and edited coefficient references but rejects incompatible directory metadata. At L=0 the output is an identity copy of the base.

## Acceptance

Constant inputs produce identical G levels and +0 details. For odd 3×5 dimensions, preserve each level shape. L=0 identity preserves signed zero. Editing one detail changes only its corresponding reconstruction scale. Numerical-error checks must not require arbitrary floats to round-trip losslessly.

## References

[S07 · Paris, Hasinoff, Kautz: Local Laplacian Filters](../research-sources.md#s07)
