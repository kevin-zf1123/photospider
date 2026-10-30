---
spec_schema_version: 1
id: FIL-12A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-12
function: hessian2d
proposed_operation_keys:
- filter.hessian2d_strict
- filter.hessian2d_accelerated_apple_silicon
- filter.hessian2d_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# FIL-12A: hessian2d

Discrete Hessian.

Inherits [FIL-12](FIL-12_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.hessian2d_strict`, `filter.hessian2d_accelerated_apple_silicon`, `filter.hessian2d_accelerated_x86_64`.

`input` → independently requestable `hxx,hxy,hyy` outputs.

### Parameters

Explicit Int64 `y_axis` and `x_axis`; explicitly supplied positive finite Float64 `dx,dy`; fixed `reflect_half` boundary with `cval=0`. Optional pre-smoothing is an explicit FIL-04B node; no sigma is hidden in this operation.

## Mathematics

`hxx=(E-2C+W)/dx²`; hyy uses the analogous y expression; `hxy=(SE-SW-NE+NW)/(4dxdy)`. Evaluate each complete expression exactly and round once to RN_t. The y axis points down.

## Behavior

`hxx` and `hyy` use the cross stencil; `hxy` reads only the four diagonal corners. Outputs have independent support.

## Acceptance

I=a*x²+b*x*y+c*y² ->(2a,b,2c) interior spacing1.
