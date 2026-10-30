---
spec_schema_version: 1
id: FIL-01B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-01
function: correlate2d
proposed_operation_keys:
- filter.correlate2d_strict
- filter.correlate2d_accelerated_apple_silicon
- filter.correlate2d_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
- S02
---
# FIL-01B: correlate2d

Two-dimensional correlation over a floating-point field.

Inherits [FIL-01](FIL-01_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.correlate2d_strict`, `filter.correlate2d_accelerated_apple_silicon`, `filter.correlate2d_accelerated_x86_64`.

`input` is a floating-point numerical field and `kernel` is a Float64 `[Kh,Kw]` array; output is `output`. Non-spatial axes and planes are preserved. Spatial shape and global output origin follow the boundary contract.

### Parameters

`y_axis`, `x_axis`, `anchor_y`, `anchor_x`, `shape`, `boundary`, `cval`, `normalization`, and `bias` are all required. `shape` is `same`, `full`, or `valid`; `boundary` is explicit. `full` and `valid` require constant extension with `cval=0`. Normalization is `none`, `sum`, or `l1`. `bias` is a finite Float64 stored value. The kernel is Float64 `[Kh,Kw]`; anchor is required, including for odd kernels. No constructor defaults.

## Mathematics

Use the correlation coordinate convention from [boundary contract](FILTER_boundary_contract.md). Set N=1 for `none`, N=ΣK for `sum`, and N=Σ|K| for `l1`; the requested normalization requires an exact nonzero divisor as an unchecked caller premise. In strict mode each sample is `RN_t(Σ K_i I_i/N + bias)`. Do not round products, sums, or division to an intermediate dtype. Correlation samples with the kernel in stored orientation and does not reverse it.

Kernel values are algorithmic controls; numerical kernel and parameter domains are unchecked caller premises. Constant-boundary taps do not read source data.

## Acceptance

The reference case uses input row `[1,2,4]`, kernel row `[1,2]`, anchor `(0,0)`, constant-zero boundary, and same shape. Correlation outputs `[5,10,4]`. Also cover off-center even kernels, full/valid global origins, legal negative sum, and zero taps that exclude poisoned source values from arithmetic.

## References

[S01](../research-sources.md#s01), [S02](../research-sources.md#s02).
