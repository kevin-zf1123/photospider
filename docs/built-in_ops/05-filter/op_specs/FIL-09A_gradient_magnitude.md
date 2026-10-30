---
spec_schema_version: 1
id: FIL-09A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-09
function: gradient_magnitude
proposed_operation_keys:
- filter.gradient_magnitude_strict
- filter.gradient_magnitude_accelerated_apple_silicon
- filter.gradient_magnitude_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# FIL-09A: gradient_magnitude

Gradient magnitude.

Inherits [FIL-09](FIL-09_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.gradient_magnitude_strict`, `filter.gradient_magnitude_accelerated_apple_silicon`, `filter.gradient_magnitude_accelerated_x86_64`.

`gx,gy` with matching shapes and dtypes → `magnitude` of the same dtype and shape.

### Parameters

`norm` is explicitly supplied as `l2` or `l1`; no constructor default.

## Mathematics

For `l2`, evaluate the exact sum of squares, then apply sqrt and round once: `RN_t(sqrt(gx²+gy²))`. For `l1`, use `RN_t(abs(gx)+abs(gy))`. Do not replace the exact expression with host floating-point squaring that can overflow prematurely.

## Behavior

At each q, depends only on the co-located `gx` and `gy`. Output units match the gradient units. NaN/Inf behavior follows the NUM operations used by the selected norm; there is no additional finite-only sample restriction.

## Acceptance

`(3,4)` yields 5 for l2 and 7 for l1. Any combination of signed zero yields +0. Large representable inputs must not overflow an intermediate when the exact result is representable.
