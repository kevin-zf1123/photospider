---
spec_schema_version: 1
id: RES-07B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: RES-07
function: anscombe_inverse_algebraic
proposed_operation_keys:
- restoration.anscombe_inverse_algebraic
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-07B: anscombe_inverse_algebraic

Algebraic Anscombe inverse.

Inherits the [RES-07 family contract](RES-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.anscombe_inverse_algebraic`.

### Ports

`stabilized` → `count_estimate`.

### Parameters

Require z≥0 and an explicit `negative_estimate=clamp|error` policy.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

Compute v=z²/4-3/8 in the input dtype; apply the selected policy to the computed v and publish v or max(v,0). This is explicitly an algebraic inverse and is not claimed to correct low-count bias.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Pointwise.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

z=2 yields 5/8. Clamping z=0 yields zero. Forward followed by inverse has two RN stages and is not guaranteed to round-trip bitwise.

## References

[S21 · Mäkitalo, Foi: inverse Anscombe research](../research-sources.md#s21)
