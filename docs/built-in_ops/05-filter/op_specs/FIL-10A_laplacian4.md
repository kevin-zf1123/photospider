---
spec_schema_version: 1
id: FIL-10A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-10
function: laplacian4
proposed_operation_keys:
- filter.laplacian4_strict
- filter.laplacian4_accelerated_apple_silicon
- filter.laplacian4_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# FIL-10A: laplacian4

Four-neighbor Laplacian.

Inherits [FIL-10](FIL-10_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.laplacian4_strict`, `filter.laplacian4_accelerated_apple_silicon`, `filter.laplacian4_accelerated_x86_64`.

`input` → numerical-field output `laplacian` with the input shape and dtype.

### Parameters

Explicit Int64 `y_axis` and `x_axis`; explicitly supplied positive finite Float64 `dx,dy`; fixed boundary `reflect_half` with `cval=0`; explicitly supplied finite Float64 `scale`. No constructor defaults.

## Mathematics

Evaluate the entire expression exactly and round once: `RN_t(scale*((E+W-2C)/dx²+(N+S-2C)/dy²))`.

## Behavior

Depends on the five-point cross. Even when scale is zero, the full arithmetic is evaluated and NaN/Inf operands propagate; this is not an identity bypass.

## Acceptance

For `I=x²+y²` and interior `dx=dy=1`, output is 4. A constant input yields zero. Verify impulse signs under the symmetric stencil.
