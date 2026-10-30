---
spec_schema_version: 1
id: RES-07C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-07
function: anscombe_inverse_mean
proposed_operation_keys:
- restoration.anscombe_inverse_mean
numeric_reference: F
oracle_scope: diagnostic_inverse_not_strict_golden
---

# RES-07C: anscombe_inverse_mean

Poisson mean-domain inverse.

Inherits the [RES-07 family contract](RES-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.anscombe_inverse_mean`.

### Ports

`denoised_stabilized_mean` → `mean_count`.

### Parameters

Require z≥0; the mean-inverse definition is fixed. Solver precision and work limits are execution resources, not quality parameters that change the mathematical result.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

The mathematical target is the inverse of family m: return +0 when z≤m(0), otherwise the unique root λ. Native evaluation must document function/tail and root tolerances for each dtype and validate against an independent higher-precision reference; exact final rounding is not required. Do not substitute the algebraic inverse or an unvalidated fit.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Pointwise.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Values at or below m(0) map to zero. A diagnostic may generate high-precision m from a known λ and invert it; low counts differ materially from the algebraic inverse.


## References

[S21 · Mäkitalo, Foi: inverse Anscombe research](../research-sources.md#s21)
