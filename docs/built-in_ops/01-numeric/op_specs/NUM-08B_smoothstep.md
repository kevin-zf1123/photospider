---
spec_schema_version: 1
id: NUM-08B
parent_id: NUM-08
function: smoothstep
proposed_operation_keys:
  - numeric.smoothstep_strict
  - numeric.smoothstep_accelerated_apple_silicon
  - numeric.smoothstep_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-08B: smoothstep

The strict profile defines the exact reference result below. Accelerated profiles follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md), including its range and fallback rules. Endpoint selections and special-value handling remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for registration and acceptance conventions. The operation has three dynamic Result inputs in order: `input`, `edge0`, and `edge1`. Each Result contains one tensor member, whose key may be any valid member key. All members must have identical `sample_shape()` and Float32 or Float64 dtype. `sample_shape()` includes batch axes; accepted rank is 1 through 8, every extent is positive, and the sample count is at most 2^40.

The output port is `values`, a Result with schema `photospider.tensor` and tensor member key `samples`. Its dtype and complete shape match the input members. Every output axis is an ordinary axis; output facets and batch axes are empty. There are no static numeric parameters or implicit casts or broadcasts. Connect scalar edges through explicit broadcast operations. This operation uses the cubic smoothstep polynomial; it does not provide quintic smootherstep.

## Numeric contract

For each logical coordinate, require finite edges with `edge0 < edge1`. Invalid edges fail with `InvalidArgument`, `FailureReason::InvalidDomain`, and `FailureScope::Run`; the diagnostic identifies the edge port, its raw bits, and the global coordinate. Validate edges before processing the corresponding input sample, so invalid edges take precedence over input NaN. Invalid edges anywhere in the full shape fail the invocation, including samples outside a consumer projection.

With valid edges, a NaN input is quieted while preserving sign and payload. An input at or below `edge0` produces positive zero, including negative infinity. An input at or above `edge1` produces positive one, including positive infinity. For an interior finite input, define

$$
t=\frac{x-edge_0}{edge_1-edge_0},\qquad s=t^2(3-2t).
$$

The output is `RN_dtype(s)`, with one final destination rounding from the exact mathematical rational. Intermediate rounded `t` or polynomial operations do not define the result. Strict output is bitwise reproducible; accelerated floating output follows the shared FP32-scaled bound and remains in [0,1]. The exact midpoint maps to 0.5. Interior values rounding to zero produce positive zero. A positive exact edge width remains valid even when naive floating subtraction of the edges would overflow.

The implementation represents finite binary64 differences in a 104-limb (6656-bit) workspace. It forms `d = x - edge0` and `w = edge1 - edge0` as exact scaled integers, then evaluates the exact rational `d²(3w-2d)/w³` with fixed-width products and final rational rounding. For accelerated profiles, the ratio workspace may form outward bounds from the leading 53 bits. It accepts a candidate only when the interval is finite, avoids subnormal and overflow ranges, and the destination-rounded candidate is within a two-Float32-ULP absolute guard of both interval endpoints. If these checks cannot certify the result, the workspace falls back to exact integer quotient rounding. Float64 inputs are not narrowed to Float32. The 6656-bit capacity bound covers binary64 edge differences, the cubic numerator, quotient alignment, and the final significand. Work includes each full logical sample and actual refinement work; temporary capacities are included in resource accounting. Exhausted memory or work budgets return `ResourceExhausted` without publishing an approximation. See `plugins/ops/01-numeric/exact_interpolation.hpp` and `exact_ratio.hpp`.

## Result execution and resources

For every nonempty request, Whole execution requests complete Data, Validation, and Descriptor support (Need role `13`) for all three input tensor members. The coordinator supplies authorized input windows before the callback. The callback computes and publishes the complete packed Result synchronously; the executor then serves requested consumer coordinates in global coordinates. Query coverage controls the observed dependency footprint, not publication shape. An input edit dirties the observed output footprint. Empty output support uses metadata only and skips payload reads and sample arithmetic.

Source Results retain their original typed backing under the execution Root, and callback read windows keep those authorized owners alive while computing. The callback receives a borrowed packed writer for its output; storage includes the complete `N × dtype width` output and a fixed interpolation workspace. Sparse consumer requests can therefore incur full-shape work and output storage. Resource or work exhaustion, cancellation, or another callback failure prevents publication and releases unpublished output and scratch. Cancellation checks occur per sample, during exact arithmetic, and before publication. A successfully published Result owns its storage independently of the execution context.

Metadata specialization rejects unsupported dtype, rank, shape, extents, or sample count before execution. Dynamic invalid edges fail at run scope. Valid input NaN and infinity follow the numeric rules above and produce an output. Other upstream, typed-validation, resource, and cancellation failures propagate through the Result execution failure path.

## Acceptance and implementation evidence

The current Result workflow uses input `[-1,0,0.25,0.5,0.75,1,2]` with broadcast edges 0 and 1 and produces `[0,0,0.15625,0.5,0.84375,1,1]`; it then mixes that output with endpoints 10 and 20, producing `[10,10,11.5625,15,18.4375,20,20]`. The focused workflow checks invalid-edge precedence outside a sparse query, all-port support and dirty mapping, Empty output, and global-coordinate projection. Shared mix cases check Straight-alpha validation on unselected channels, signaling-NaN endpoint bit copying, and retained output lifetime; smoothstep-specific checks include cubic execution with caller and worker floating-environment restoration. The separate `test_numeric_result_math_sequences.cpp` integration fixture retains additional infinity/NaN, batch-axis, negative-stride and pre-cancellation cases; it was not rerun for this Result update. See [NUM-08 Result Whole execution](../interpolation-whole.md) for current commands and evidence.

The three versioned keys implement the dynamic-edge contract through synchronous Result Whole execution. The existing `field.smoothstep` remains a separate rank-2 operation with static Float64 edges and Float32 coverage output. The current strict and Apple Silicon oracle, root test, and installed consumer results are summarized in [NUM-08 Result Whole execution](../interpolation-whole.md). No x86 execution, native GPU support or performance result is claimed. The specification remains Proposed; implementation status describes code presence, not specification acceptance or cross-platform validation.
