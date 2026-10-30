---
spec_schema_version: 1
id: FIL-08A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-08
function: central_gradient
proposed_operation_keys:
- filter.central_gradient_strict
- filter.central_gradient_accelerated_apple_silicon
- filter.central_gradient_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# FIL-08A: central_gradient

Central gradient.

Inherits [FIL-08](FIL-08_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.central_gradient_strict`, `filter.central_gradient_accelerated_apple_silicon`, `filter.central_gradient_accelerated_x86_64`.

`input` → independently requestable `gx,gy`, each with the input shape. No complete-color assertion is made.

### Parameters

Explicit Int64 `y_axis` and `x_axis` values; positive finite Float64 `spacing_x` and `spacing_y`; explicitly selected `normalization` (`unit_ramp` or `raw`). Boundary is fixed by the named profile to `reflect_half`, with `cval=0`. The constructor supplies no defaults.

## Mathematics

Use correlation with x kernel [-1,0,1] and a center-only kernel on the other axis; `gy` is the transpose. For `unit_ramp`, `gx=RN_t(ΣKx I/(2*spacing_x))` with the analogous y expression. `raw` omits division by 2 but still divides by spacing. Each component is evaluated exactly and rounded once; the zero tap is not read.

## Behavior

`gx` and `gy` have independent sparse support; a request for `gx` alone depends only on the `gx` support. Spacing is a static parameter.

## Acceptance

For an interior ramp I=x, `unit_ramp` gives gx=1/dx and gy=0; verify the positive y direction. A one-pixel clamped input yields zero. Include separate poisoned-input tests for the two outputs and boundary fixtures.
