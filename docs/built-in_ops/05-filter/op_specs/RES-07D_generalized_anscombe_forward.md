---
spec_schema_version: 1
id: RES-07D
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: RES-07
function: generalized_anscombe_forward
proposed_operation_keys:
- restoration.generalized_anscombe_forward
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-07D: generalized_anscombe_forward

Generalized Anscombe forward transform.

Inherits the [RES-07 family contract](RES-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.generalized_anscombe_forward`.

### Ports

`observation` → `stabilized`.

### Parameters

Require gain a>0, finite offset b, and read_sigma s≥0.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

Evaluate `(2/a)*sqrt(max(a*(y-b)+(3/8)*a²+s²,0))` with native input-dtype arithmetic; a zero radicand yields +0. y may be signed; max(.,0) is an explicit model rule, not validation. Native NaN propagation follows RES_native_contract.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Pointwise.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

With a=1, b=0, s=0, and y≥0 this reduces to member A. A sufficiently low y maps to zero. A unit change requires corresponding changes to a, b, and s.

## References

[S21 · Mäkitalo, Foi: inverse Anscombe research](../research-sources.md#s21)
