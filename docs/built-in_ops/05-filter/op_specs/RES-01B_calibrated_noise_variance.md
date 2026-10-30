---
spec_schema_version: 1
id: RES-01B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: RES-01
function: calibrated_noise_variance
proposed_operation_keys:
- restoration.calibrated_noise_variance
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-01B: calibrated_noise_variance

Calibrated Poisson–Gaussian variance field.

Inherits the [RES-01 family contract](RES-01_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.calibrated_noise_variance`.

### Ports

`mean_signal` is a Float32/Float64 tensor of logical rank 1–8. It produces `variance,sigma`, both with the same shape and dtype. Participating mean values are finite model inputs, an unchecked caller premise. Native intermediate/output overflow follows IEEE arithmetic.

### Parameters

Required real parameters are `gain` a>0, finite `offset` b, and `read_sigma` s≥0, with units defined by y=a*P+b+N(0,s²). Required string `below_offset=clamp|error` chooses the mathematical domain. `clamp` applies the stated zero clamp. `error` means the caller guarantees mean_signal>=b; it does not request a production payload-validation scan, and violating that premise has no guaranteed error diagnostic. Parameters are converted to the input dtype t once; callers guarantee representability and the stated finite domain.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

The ideal model is λ=(mean_signal-b)/a, variance=a²*max(λ,0)+s². Native evaluation eliminates the division: compute delta=max(mean_signal-b,0), read2=s*s, and variance=fma(a,delta,read2) in dtype t. Compute sigma=sqrt(variance) from that native variance, including when both outputs are requested. The input estimates expected signal; it does not claim to be the true mean of a single noisy observation.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Pointwise. The sigma output is the square root of the same native variance value, whichever outputs are requested; a variance-only request omits the square root. No output subset introduces a domain-validation scan.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

For mean_signal=b+4a and s=0, variance=4a² and sigma=2a. Under below-offset clamping, only the read-noise variance remains.

## References

[S22 · scikit-image restoration](../research-sources.md#s22);[S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)
