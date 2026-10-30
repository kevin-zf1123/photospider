---
spec_schema_version: 1
id: FIL-04A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-04
function: gaussian_coefficients
proposed_operation_keys:
- filter.gaussian_coefficients_strict
- filter.gaussian_coefficients_accelerated_apple_silicon
- filter.gaussian_coefficients_accelerated_x86_64
numeric_reference: B
oracle_scope: mathematical_reference
research_sources:
- S03
- S25
---
# FIL-04A: gaussian_coefficients

Generate baked64 one-dimensional Gaussian coefficients.

Inherits [FIL-04](FIL-04_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.gaussian_coefficients_strict`, `filter.gaussian_coefficients_accelerated_apple_silicon`, `filter.gaussian_coefficients_accelerated_x86_64`.

There is no dynamic data input. Outputs `kernel_x` and `kernel_y` are Float64 vectors of lengths `2*radius_x+1` and `2*radius_y+1`.

### Parameters

Require finite `sigma_x` and `sigma_y` with values ≥0, integer `radius_x` and `radius_y` with values ≥0, and explicit Int64 output axes `x_axis=0` and `y_axis=0`. Each axis parameter addresses its own rank-one output; the two axes are not indices into one shared tensor. On any axis where sigma is zero, radius must be zero. A positive sigma may use radius zero, producing a one-tap discrete kernel. All are explicit; constructors do not infer radius or provide defaults.

## Mathematics

For each axis, `g_s[j]=RN64(exp(-j²/(2*s²)))` for `j=-r…r`; when `s=0`, the kernel is `[1]`. The center is exactly 1. Mirrored positions reuse identical bits. Return unnormalized coefficients; do not form and re-round the outer product of two already rounded kernels.

There is no source-data input; descriptors and static parameters determine every output. Each coefficient depends on its axis sigma and radius. Each coefficient is the correctly rounded RN64 value; tiny coefficients may underflow to positive zero.

## Acceptance

For sigma zero and radius zero, return `[1]`. For sigma 1 and radius 1, return `[RN64(exp(-1/2)),1,the same bits as the first value]`. Large radii may include weights that round to zero. Negative sigma and a nonzero radius paired with zero sigma are invalid.

## References

[S03](../research-sources.md#s03), [S25](../research-sources.md#s25).
