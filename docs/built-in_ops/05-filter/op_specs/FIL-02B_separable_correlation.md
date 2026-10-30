---
spec_schema_version: 1
id: FIL-02B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-02
function: separable_correlation
proposed_operation_keys:
- filter.separable_correlation_strict
- filter.separable_correlation_accelerated_apple_silicon
- filter.separable_correlation_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
---
# FIL-02B: separable_correlation

Separable two-dimensional correlation.

Inherits [FIL-02](FIL-02_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.separable_correlation_strict`, `filter.separable_correlation_accelerated_apple_silicon`, `filter.separable_correlation_accelerated_x86_64`.

Inputs are `input`, `kernel_x`, and `kernel_y`; output is `output`. Both kernels are positive-length Float64 vectors. Output shape and global origin follow the explicit `same|full|valid` selection in [boundary geometry](FILTER_boundary_contract.md); only `same` preserves input shape.

### Parameters

Both one-dimensional kernels are positive-length Float64 arrays. Require `y_axis`, `x_axis`, both anchors, shape (`same`, `full`, or `valid`), boundary, `cval`, normalization (`none`, `sum`, or `l1`), and finite Float64 bias. Full/valid require constant boundary with `cval=0`. Constructors have no defaults.

## Mathematics

The exact mathematical kernel is `K[j,i]=ky[j]*kx[i]`; it need not be materialized as a Float64 2D array. Apply the direction, denominator, and one final RN_t from [FIL-01B](FIL-01B_correlate2d.md). Sum normalization uses `(Σkx)(Σky)`; l1 uses `(Σ|kx|)(Σ|ky|)`.

The support is the Cartesian product of the nonzero taps on both axes after boundary mapping. All kernel values are control data. Constant extension is that of the 2D field; `cval` is not a filtered row value. Intermediates are exact. Ordinary same-dtype two-pass rounding defines a different S operator.

## Acceptance

With `kx=[1,2]` and `ky=[1,3]`, compare with direct application of the exact outer-product kernel. Include nonzero constant extension, non-unit normalization, corner samples that detect incorrect two-pass extension, and Float32 cancellation that detects intermediate rounding.

## References

[S01](../research-sources.md#s01).
