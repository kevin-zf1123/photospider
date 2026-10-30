---
spec_schema_version: 1
id: RES-07F
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-07
function: generalized_anscombe_inverse_mean
proposed_operation_keys:
- restoration.generalized_anscombe_inverse_mean
numeric_reference: F
oracle_scope: diagnostic_inverse_not_strict_golden
---

# RES-07F: generalized_anscombe_inverse_mean

Poisson–Gaussian mean-domain inverse.

Inherits the [RES-07 family contract](RES-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.generalized_anscombe_inverse_mean`.

### Ports

`denoised_stabilized_mean` → `noiseless_mean_observation`.

### Parameters

Use a, b, and s as defined for member D; require z≥0.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

The mathematical target is the inverse of family mρ. Solve for λ with documented dtype-aware function/tail and root tolerances, then evaluate b+aλ natively. On the lower branch return the input-dtype conversion of b. The s=0 mathematical target agrees with the Poisson inverse. No exact-expression or correctly-rounded-root guarantee is required.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Pointwise.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

ρ=0 reduces to the Poisson case. The mρ(0) threshold at λ=0 differs from Anscombe. A finite ±3σ Gaussian window is not an exact tail treatment.


## References

[S21 · Mäkitalo, Foi: inverse Anscombe research](../research-sources.md#s21)
