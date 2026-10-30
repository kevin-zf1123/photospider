---
spec_schema_version: 1
id: FIL-11C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-11
function: difference_of_gaussians
proposed_operation_keys:
- filter.difference_of_gaussians_strict
- filter.difference_of_gaussians_accelerated_apple_silicon
- filter.difference_of_gaussians_accelerated_x86_64
numeric_reference: S
oracle_scope: mathematical_reference
---

# FIL-11C: difference_of_gaussians

Two-scale Gaussian difference.

Inherits [FIL-11](FIL-11_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.difference_of_gaussians_strict`, `filter.difference_of_gaussians_accelerated_apple_silicon`, `filter.difference_of_gaussians_accelerated_x86_64`.

`input` → `output`.

### Parameters

Explicit positive finite Float64 `sigma_small`; explicit finite Float64 `ratio>1`; explicitly supplied Int64 radii for both Gaussian filters; explicit Int64 spatial axes; fixed `reflect_half` boundary and `cval=0`; explicit finite Float64 `gain`. No constructor defaults.

The explicit radius parameters are `radius_small` and `radius_large`. Each radius applies to both spatial axes of its corresponding Gaussian stage. Both stages use the baked64 FIL-04B kernel and strict destination-dtype rounding. The final expression applies `gain` to the exact difference of those rounded stage outputs, followed by one destination-dtype rounding.

## Mathematics

Numerical classification: **S**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

Derive the larger sigma at the explicit rounding point `RN64(sigma_small*ratio)`. The two FIL-04B outputs are each rounded to RN_t; the final output is `RN_t(gain*(Gsmall-Glarge))`. This S profile rounds both Gaussian stages applied to the same input and does not claim equivalence to LoG.

## Behavior

Support is the union of the two Gaussian footprints. The final stage at q uses the corresponding rounded stage values.

## Acceptance

A constant input yields zero. The ratio satisfies its numerical caller premise. Include a fixture sensitive to intermediate rounding when Gsmall and Glarge are nearly equal.
