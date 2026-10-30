---
spec_schema_version: 1
id: RES-01A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: RES-01
function: noise_mad_highpass
proposed_operation_keys:
- restoration.noise_mad_highpass
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-01A: noise_mad_highpass

High-pass MAD noise scale.

Inherits the [RES-01 family contract](RES-01_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.noise_mad_highpass`.

### Ports

`input,mask` → `sigma`. Input is Float32 or Float64, rank 2–8; explicit distinct `y_axis,x_axis` select the spatial plane. Other dimensions identify independent planes in their original order. Output shape is those nonspatial dimensions followed by a terminal extent 1, with Float64 dtype; a rank-2 image therefore produces `[1]`. `mask` is UInt8 with the same complete input shape and values 0/1. It is explicit, not a hidden optional input; connect an all-ones mask when no mask is used.

### Parameters

Required Int64 `y_axis,x_axis`; fixed high-pass kernel `[1,-1;-1,1]/2`, evaluated only at valid 2×2 positions; `min_samples` Int64≥1 and explicitly supplied; even-sample median uses the fixed mean rule.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

In input dtype t, compute r(y,x)=((I00-I01)-(I10-I11))*0.5. A residual is selected only when all four source mask values are 1. Compute the exact middle rank(s) of these native residual values. For an even count, average the two middle values in dtype t, avoiding avoidable overflow; this native midpoint is m. Form native d_i=abs(r_i-m) and take the same median of d. For c=Φ^-1(.75), form scale64=RN64(1/c) once and publish sigma=Float64(MAD)*scale64 with native Float64 multiplication. The reciprocal is correctly rounded from the mathematical quantile; do not use the truncated 0.6745. Participating input/residual/deviation values must be finite; this is an unchecked numerical premise. The residual kernel has L2 norm 1, but texture or correlated noise can bias the estimate; it is not claimed unbiased.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

The estimator consumes the complete plane mask as algorithmic control. Only patches whose four mask values are all 1 participate in image arithmetic. The count does not change output shape; fewer than min_samples is the explicitly defined insufficient-samples outcome on otherwise valid input, not mask-encoding validation.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Constant/ramp input gives zero residual and sigma; checker texture is a negative example where texture may be mistaken for noise. Zero selected samples or fewer than min_samples yields OperationFailed with reason InvalidDomain. This includes a valid descriptor with H=1 or W=1 (such as 1×65536), which has no valid 2×2 patch. A zero mask entry must not read poisoned source data.

## References

[S22 · scikit-image restoration](../research-sources.md#s22);[S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)
