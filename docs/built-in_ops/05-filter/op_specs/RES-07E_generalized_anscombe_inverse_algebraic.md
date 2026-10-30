---
spec_schema_version: 1
id: RES-07E
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: RES-07
function: generalized_anscombe_inverse_algebraic
proposed_operation_keys:
- restoration.generalized_anscombe_inverse_algebraic
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-07E: generalized_anscombe_inverse_algebraic

Generalized algebraic inverse.

Inherits the [RES-07 family contract](RES-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.generalized_anscombe_inverse_algebraic`.

### Ports

`stabilized` → `noiseless_mean_observation`.

### Parameters

Use a, b, and s as defined for member D; require z≥0 and an explicit `negative_estimate=clamp|error` policy.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

Compute λ=z²/4-3/8-(s/a)² and apply the policy when λ<0. Output RN_t(b+a*λ), using λ=0 when clamped; thus zero latent signal returns offset b, not zero. Subtract the offset externally to obtain signal.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Pointwise.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

With s=0, a=1, and b=0 this reduces to member B. At λ=0 return b. Do not round λ before multiplying by gain.

## References

[S21 · Mäkitalo, Foi: inverse Anscombe research](../research-sources.md#s21)
