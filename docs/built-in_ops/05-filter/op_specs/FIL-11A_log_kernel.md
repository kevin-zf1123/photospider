---
spec_schema_version: 1
id: FIL-11A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-11
function: log_kernel
proposed_operation_keys:
- filter.log_kernel_strict
- filter.log_kernel_accelerated_apple_silicon
- filter.log_kernel_accelerated_x86_64
numeric_reference: B
oracle_scope: mathematical_reference
---

# FIL-11A: log_kernel

Sampled LoG kernel with zero-DC correction.

Inherits [FIL-11](FIL-11_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.log_kernel_strict`, `filter.log_kernel_accelerated_apple_silicon`, `filter.log_kernel_accelerated_x86_64`.

No dynamic input → Float64 `kernel` with shape `[2r+1,2r+1]` and Float64 `dc_correction` with shape `[1]`.

### Parameters

Explicit positive finite Float64 `sigma`; explicit Int64 `radius≥1`; explicit Bool `scale_normalized`; fixed spacing of one pixel. No constructor defaults.

## Mathematics

Numerical classification: **B**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

For each tap, `v=RN64(((x²+y²-2sigma²)/sigma^4)*exp(-(x²+y²)/(2sigma²))/(2πsigma²) * (scale_normalized?sigma²:1))`; evaluate the complete real expression and round once to RN64. Store v in `kernel`. Expose `dc_correction=RN64(Σv)` as a separate diagnostic port. For strict zero-DC filtering, do not use this rounded correction to define B; use the exact sum of stored kernel values in the center-difference formula.

## Behavior

Sigma and radius are static parameters. Symmetric taps share identical bits.

Do not normalize the sum even for small kernels.

## Acceptance

The kernel is mirror-symmetric with a negative center. `dc_correction` may be nonzero; the rounded tap values are not guaranteed to sum to exact zero.
