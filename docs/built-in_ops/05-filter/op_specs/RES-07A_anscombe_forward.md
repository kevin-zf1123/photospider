---
spec_schema_version: 1
id: RES-07A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: RES-07
function: anscombe_forward
proposed_operation_keys:
- restoration.anscombe_forward
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-07A: anscombe_forward

Anscombe forward transform.

Inherits the [RES-07 family contract](RES-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.anscombe_forward`.

### Ports

`count` → `stabilized` with the same shape and dtype.

### Parameters

Require count≥0; there is no implicit gain or unit conversion.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

Evaluate `2*sqrt(count+3/8)` with native input-dtype arithmetic. At count=0 the result is sqrt(3/2), not zero.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Pointwise. The nonnegative input domain is an unchecked caller premise.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Analytic fixtures at count=0, 1, and 3/8; behavior for tiny count changes; no unnecessary intermediate overflow for very large counts.

## References

[S21 · Mäkitalo, Foi: inverse Anscombe research](../research-sources.md#s21)
