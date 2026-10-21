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

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Scope and interface

Coordinate shapers map generic Float32/Float64 arrays to LUT coordinates or back to an explicit interval. The linear pair are authoring templates built from fixed NUM-06 remap and constant operations. The log2 pair are six independently named CPU primitive keys. No tone mapping or color-description conversion is inferred.

All four interfaces take Result inputs `input`, `lower` and `upper`. Each has one tensor member under any schema id/version/key. Full `sample_shape()` includes batch axes. Input has rank 1..8, positive extents and at most 2^40 elements; bounds have shape [1] and the same dtype as input. Output `values` uses `photospider.tensor` v1/member `samples`, preserves complete input shape and dtype as ordinary axes, and carries no domain description. The compiler validates these static requirements, including Empty requests.

Linear bounds must be finite and satisfy lower<upper. Log bounds must be finite and satisfy 0<lower<upper. These are unchecked caller preconditions, including when the input contains IEEE special values. Invalid-bound diagnostics and error precedence are outside the contract. Log bit decomposition retains a nonzero-significand guard for safe ctz/clz and integer division; internal divisor/capacity guards also remain. These guards do not validate the complete numerical bound condition.

The log primitives accept no static mode, clipping or dtype parameter. Their exact keys select direction and CPU profile. Linear templates accept an authoring profile (strict by default) and author the corresponding ordinary graph nodes. Construction performs no payload computation.

## Mathematical formulas and IEEE results

Forward linear is (x-L)/(U-L); inverse linear is L+t*(U-L). Log forward is (log2(x)-log2(L))/(log2(U)-log2(L)); log inverse is L*(U/L)^t. Each strict operation correctly rounds its complete mathematical formula once to the output dtype. Separately rounded logarithms, quotients or powers do not define the log reference. In particular, an overflowing rounded U/L must not change the inverse result.

All forms extend beyond their nominal intervals without implicit clipping. Positive log inputs may lie outside [L,U]; inverse finite coordinates may lie outside [0,1]. Callers requiring clipping compose an explicit clamp.

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

Apple Silicon and x86 accelerated profiles follow the [final FP32 four-ULP contract](NUM_accelerated_contract.md), retaining strict endpoint, NaN/Inf/zero classifications and signs. For fixed valid bounds the complete fast/certified mapping must remain monotone nondecreasing over its ordered real domain and independent of request partition/order. Per-point ULP checks alone do not prove that property. Current log profiles all correctly round the same exact/certified reference and therefore preserve monotonicity without batch sorting. Unsupported CPU profiles fail during preparation; these keys do not promise GPU execution.

## Fixed log execution

Each selected output tile is one fixed CPU Host task. It reads the matching input region and both scalar bounds, closed through each source's own grid. Atomic or batch source axes may enlarge physical coverage. The task uses global coordinates, evaluates only selected cells and writes a preallocated output tile. Bounds remain payload dependencies for endpoint and special-value queries because selection is data-dependent. Required upstream producers must meet their own contracts for their declared output tiles.

Preparation verifies all metadata, direction symbol and kernel contract, empty static/task parameter sets, exact scratch size, canonical output geometry and exact input tile slots. Prepared views support legal signed/zero strides and unaligned offsets. DirectTiles retains matching source views; DenseHost imports have explicit Plan COPY tasks and their storage. Empty selection emits no payload task or read.

Each invocation owns one preallocated ExactShaper state and O(rank) coordinate state; scratch is independent of the complete logical shape. Output storage covers selected tiles. Physical input descriptors and retained views scale with the declared closure. Work is O(selected cells) plus bounded exact/certified refinement. Runs share immutable preparation and have separate arithmetic/output storage; compatible source/bound replacements reuse preparation.

## Arithmetic state and operational failures

The log engine retains dyadic exact branches and directed logarithm/exponential enclosures at 128,256,...,4096 fractional bits. Forward divides complete log differences. Inverse encloses ln(L)+t*(ln(U)-ln(L)), range-classifies that complete value and carries the power-of-two scale through final rounding. Both enclosure endpoints must round to the same output bits. Unresolved precision or arena capacity returns ResourceExhausted/CapacityLimit.

This engine uses a checked control-flow instantiation of the shared interval algorithms. Retry, overflow, underflow and terminal failure are fixed state fields, independent of numerical sample validation. A precision retry cannot overwrite cancellation or another terminal failure. Overflow/underflow yield the specified successful IEEE values. Scratch includes the arithmetic pool and failure-propagation slots; range results, refinement and cancellation do not allocate exception objects. Other instantiations of the shared interval algorithms retain their own error protocol.

FloatingEnvironment preserves caller state and establishes RN/gradual underflow. Cancellation is checked per output and in extended arithmetic. Static/schema, binding/layout, backend, resource and cancellation failures retain their operational categories. Successful tile writes become internal dependencies; public call Results appear after all required tasks succeed and the call retires in order. A failed call cannot publish successful partial outputs. Retained output owners survive source/context teardown. Content caching, repeated Result identity and per-value fallback counters are outside this contract.

## Linear composition

The linear helper expands lower/upper and scalar 0/1 into fixed constant output tiles consumed by remap_range: forward source=[lower,upper], target=[0,1]; inverse source=[0,1], target=[lower,upper]. Finite ordered bounds are caller preconditions. Both forms compute selected dense tiles and admit their selected intermediates.

The inverse retains a scalar remap `remap_range(input=lower,source_lower=lower,source_upper=upper,target_lower=lower,target_upper=lower)` before expanding target_lower. This preserves bound dependencies and lower bits under the valid-domain precondition, without a runtime order assertion. Float32 literal creation uses fixed count-one linspace. No separate linear primitive is registered.

## Acceptance and implementation

With L=1,U=16, log forward [0.5,1,2,4,16,32] gives [-0.25,0,0.25,0.5,1,1.25], and inverse recovers those exact powers. General rounded forward/inverse composition is not a bitwise identity. Linear L=-2,U=2 maps [-2,0,2,4] to [0,0.5,1,1.5].

Use the retained [Fraction and directed-MPFR reference](../../../../oracle/ops/numeric/crv08/reference.py). Log reference methods are 2 and 3; filter invalid bounds for valid numerical checks, but retain query NaN/Inf/negative/zero and successful output Inf/zero. Do not substitute individually rounded host log/pow calls. Cover near-equal/extreme bounds, exact root/midpoint branches, endpoints, payload-preserving NaN quieting, subnormals and complete-expression overflow/underflow. Verify monotonicity by fixed-bound groups across exact/certified branches and request partitions.

Temporary public tests must separately exercise selected tile support, both scalar dependencies, ordinary/batch/atomic schemas, affine layouts, Empty, malformed static/prepare descriptions, compatible rebinding, concurrent Runs, fenv, cancellation inside refinement, scratch/output admission and retained owners. Check huge sparse logical input by actual execution. Direct arithmetic probes should verify successful range, retry/capacity and cancellation transitions without exception allocation, while maintaining the default interval instantiation's behavior. Remove temporary checkers after validation; retain independent oracles. Report only CPU profiles and cases actually executed.

`log2_shaper_node` and `log2_shaper_inverse_node` author the primitive nodes. `log_shaper.cpp` provides fixed lowering/preparation/invocation; `exact_shaper.hpp` owns complete-expression arithmetic. The separate `linear_shaper` and `linear_shaper_inverse` helpers author ordinary fixed graph compositions.

- [Linear template](CRV-08A_linear_shaper.md).
- [Inverse linear template](CRV-08B_linear_shaper_inverse.md).
- [Log2 primitive](CRV-08C_log2_shaper.md).
- [Inverse log2 primitive](CRV-08D_log2_shaper_inverse.md).
- [Curve category](../curves.md).
- [NUM-06 remap_range](NUM-06B_remap_range.md).
