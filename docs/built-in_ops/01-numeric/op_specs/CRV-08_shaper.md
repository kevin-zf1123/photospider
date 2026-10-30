---
spec_schema_version: 1
id: CRV-08
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-08: scalar coordinate shapers

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Interface

Coordinate shapers map generic Float32/Float64 arrays to LUT coordinates or back to an explicit interval. The linear pair are authoring templates built from NUM-06 remap and constant operations. The log2 pair are six independently named CPU primitive keys (forward and inverse, each with strict, apple_silicon and x86_64 profiles). No tone mapping or color-description conversion is inferred.

All four interfaces take Result inputs `input`, `lower` and `upper`. Each has one tensor member under any schema id/version/key. Full `sample_shape()` includes batch axes. Input has rank 1..8, positive extents and at most 2^40 elements; bounds have shape [1] and the same dtype as input. Output `values` uses `photospider.tensor` v1/member `samples`, preserves complete input shape and dtype as ordinary axes, and carries no domain description.

The log primitives accept no static mode, clipping or dtype parameter. Their exact keys select direction and CPU profile. Linear templates accept an authoring profile (strict by default) and author the corresponding ordinary graph nodes. The authoring helpers are `log2_shaper_node`, `log2_shaper_inverse_node`, `linear_shaper` and `linear_shaper_inverse`. Unsupported CPU profiles fail during preparation; these keys do not promise GPU execution.

## Mathematics

Forward linear is (x-L)/(U-L); inverse linear is L+t*(U-L). Log forward is (log2(x)-log2(L))/(log2(U)-log2(L)); log inverse is L*(U/L)^t. Each strict operation correctly rounds its complete mathematical formula once to the output dtype. Separately rounded logarithms, quotients or powers do not define the log reference. In particular, an overflowing rounded U/L must not change the inverse result.

All forms extend beyond their nominal intervals without implicit clipping. Positive log inputs may lie outside [L,U]; inverse finite coordinates may lie outside [0,1]. Callers requiring clipping compose an explicit clamp.

Linear bounds must be finite and satisfy lower<upper. Log bounds must be finite and satisfy 0<lower<upper. These are unchecked caller preconditions, including when the input contains IEEE special values. Invalid-bound diagnostics and error precedence are outside the contract.

For valid bounds, input NaNs retain sign and payload and signaling NaNs are quieted. Linear forms inherit remap_range's IEEE infinity, overflow and zero semantics. The log pair specifies:

| Input | Forward | Inverse |
| --- | --- | --- |
| +0 or -0 | -Inf | exact L bits |
| 1 | formula, or exact endpoint if applicable | exact U bits |
| negative finite | canonical quiet NaN | positive mathematical result, possibly rounded to +0/Inf |
| -Inf | canonical quiet NaN | +0 |
| +Inf | +Inf | +Inf |
| x=L or x=U | exact +0 or 1 | formula unless t=0/1 |

Numerical overflow and underflow are successful IEEE results. Log inverse's finite exact value is positive, so underflow zero is +0. The linear inverse preserves like-signed infinities; its negative-infinity result is independent of the log inverse's +0 extension.

Apple Silicon and x86 accelerated profiles follow the [final FP32 four-ULP contract](NUM_accelerated_contract.md), retaining strict endpoint, NaN/Inf/zero classifications and signs. For fixed valid bounds the complete accelerated mapping is monotone nondecreasing over its ordered real domain and independent of request partition/order.

## Behavior

Each log output element depends on the matching input element and both scalar bounds. Bounds remain dependencies for endpoint and special-value queries because the result selection is data-dependent. Region and tile results equal the corresponding slice of the whole result.

## Conceptual DAG: linear composition

The linear helper expands lower/upper and scalar 0/1 into constant outputs consumed by remap_range: forward source=[lower,upper], target=[0,1]; inverse source=[0,1], target=[lower,upper]. Finite ordered bounds are caller preconditions.

The inverse retains a scalar remap `remap_range(input=lower,source_lower=lower,source_upper=upper,target_lower=lower,target_upper=lower)` before expanding target_lower. This preserves bound dependencies and lower bits under the valid-domain precondition, without a runtime order assertion. Float32 literal creation uses count-one linspace. No separate linear primitive is registered.

## Acceptance

With L=1,U=16, log forward [0.5,1,2,4,16,32] gives [-0.25,0,0.25,0.5,1,1.25], and inverse recovers those exact powers. General rounded forward/inverse composition is not a bitwise identity. Linear L=-2,U=2 maps [-2,0,2,4] to [0,0.5,1,1.5].

Expected log values come from exact (arbitrary-precision) evaluation of the complete formula, not individually rounded host log/pow calls. Filter invalid bounds for valid numerical checks, but retain query NaN/Inf/negative/zero and successful output Inf/zero. Cover near-equal/extreme bounds, exact root/midpoint branches, endpoints, payload-preserving NaN quieting, subnormals and complete-expression overflow/underflow. Verify monotonicity by fixed-bound groups across request partitions.

## References

- [Linear template](CRV-08A_linear_shaper.md).
- [Inverse linear template](CRV-08B_linear_shaper_inverse.md).
- [Log2 primitive](CRV-08C_log2_shaper.md).
- [Inverse log2 primitive](CRV-08D_log2_shaper_inverse.md).
- [Curve category](../curves.md).
- [NUM-06 remap_range](NUM-06B_remap_range.md).
