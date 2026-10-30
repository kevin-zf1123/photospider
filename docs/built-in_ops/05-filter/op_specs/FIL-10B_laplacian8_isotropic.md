---
spec_schema_version: 1
id: FIL-10B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-10
function: laplacian8_isotropic
proposed_operation_keys:
- filter.laplacian8_isotropic_strict
- filter.laplacian8_isotropic_accelerated_apple_silicon
- filter.laplacian8_isotropic_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# FIL-10B: laplacian8_isotropic

Nine-point isotropic approximate Laplacian.

Inherits [FIL-10](FIL-10_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.laplacian8_isotropic_strict`, `filter.laplacian8_isotropic_accelerated_apple_silicon`, `filter.laplacian8_isotropic_accelerated_x86_64`.

`input` → `laplacian`.

### Parameters

Explicit Int64 `y_axis` and `x_axis`; explicitly supplied positive finite Float64 spacing `s`. Anisotropic dx/dy is not supported. Boundary is fixed by `nine_point_iso_v1` to `reflect_half` with `cval=0`. Explicit finite Float64 `scale`; no constructor defaults.

## Mathematics

`RN_t(scale*(4*(N+S+E+W)+(NE+NW+SE+SW)-20*C)/(6*s²))`. This is the named `nine_point_iso_v1` profile, not an arbitrary equal-weight sum over eight neighbors.

## Behavior

Depends on all nine samples in the 3x3 footprint.

## Acceptance

For `I=x²+y²`, output is `4/s²`; constant input yields zero. This differs from `[1,1,1;1,-8,1;1,1,1]`.
