---
spec_schema_version: 1
id: NUM-08A
parent_id: NUM-08
function: mix
proposed_operation_keys:
  - numeric.mix_strict
  - numeric.mix_accelerated_apple_silicon
  - numeric.mix_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-08A: mix

The strict profile defines the exact reference result below. Accelerated profiles follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md), including its range and fallback rules. Discrete selections and special-value handling remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for registration and acceptance conventions. The operation has three dynamic Result inputs in order: `a`, `b`, and `t`. Each Result contains one tensor member, whose key may be any valid member key. The three members must have identical `sample_shape()` and Float32 or Float64 dtype. `sample_shape()` includes batch axes; accepted rank is 1 through 8, every extent is positive, and the sample count is at most 2^40.

The output port is `values`, a Result with schema `photospider.tensor` and tensor member key `samples`. Its dtype and complete shape match the input members. Every output axis is an ordinary axis; output facets and batch axes are empty. The operation does not apply implicit broadcasting, casts, color-space conversion, alpha association, or image compositing. Connect a common scalar factor through an explicit broadcast operation.

## Numeric contract

For each logical coordinate, the mathematical blend is

$$
\operatorname{mix}(a,b,t)=(1-t)a+tb.
$$

The factor `t` must be finite and satisfy `0 <= t <= 1`. Negative zero is a valid zero. A non-finite or out-of-range factor fails with `InvalidArgument`, `FailureReason::InvalidDomain`, and `FailureScope::Run`; the diagnostic identifies port 2, the raw factor bits, and the global coordinate. Invalid factors anywhere in the full input shape fail the invocation, including samples outside a consumer projection.

At `t = 0`, copy `a`'s bits; at `t = 1`, copy `b`'s bits. These selections preserve signaling-NaN and signed-zero encodings without floating-point evaluation. Both source inputs remain required and are completely read and typed-validated before the callback, including the unselected endpoint. Consequently, an upstream or typed-input failure on either endpoint can fail the operation before factor validation begins.

For `0 < t < 1`, both endpoint samples participate. If either endpoint is NaN, return the first NaN in input order, quieting it while preserving sign and payload. If an endpoint is infinite, return that infinity when it is the only infinity or both infinities have the same sign; opposite-sign infinities produce the fixed positive quiet NaN. These cases produce numeric output successfully.

For finite endpoints and an interior factor, round the exact real expression directly once to the output dtype. Strict results are bitwise reproducible. Accelerated floating results use the shared FP32-scaled error bound. Two negative-zero endpoints produce negative zero; any other exact zero interior result produces positive zero. A nonzero exact result that underflows to zero keeps its mathematical sign. Since `t` is in [0,1], finite endpoints cannot produce an exact result outside their range. The implementation must avoid intermediate subtraction or products that introduce spurious overflow.

## Result execution and resources

For every nonempty request, Whole execution requests complete Data, Validation, and Descriptor support (Need role `13`) for all three input tensor members. The coordinator supplies authorized input windows before the callback. The callback computes and publishes a complete packed Result synchronously; the executor then serves requested consumer coordinates in global coordinates. Query coverage controls the observed dependency footprint, not publication shape. An edit to an input dirties the observed output footprint. Empty output support uses metadata only and skips payload reads and sample arithmetic.

Source Results retain their original typed backing under the execution Root, and callback read windows keep those authorized owners alive while computing. The callback receives a borrowed packed writer for its output; storage includes the complete `N × dtype width` output and a fixed interpolation workspace. Sparse consumer requests can therefore incur full-shape work and output storage. Resource or work exhaustion, cancellation, or another callback failure prevents publication and releases unpublished output and scratch. Cancellation checks occur per sample, during exact arithmetic, and before publication. A successfully published Result owns its storage independently of the execution context.

## Acceptance and implementation evidence

The current public Result workflow composes seven smoothstep samples `input=[-1,0,0.25,0.5,0.75,1,2]` using broadcast edges 0 and 1 with mix endpoints 10 and 20. The mix output is `[10,10,11.5625,15,18.4375,20,20]`. The focused workflow checks endpoint source failures for both branches, full support and sparse dirty mapping, typed validation of an unselected endpoint, Empty support, cache association/invalidation, and output readability after context retirement. The broader `test_numeric_result_math_sequences.cpp` integration fixture retains separate batch-axis, negative-stride, special-value and pre-cancellation cases; it was not rerun for this Result update. The related image operation `image.mix` is covered separately by [test_result_image_composite.cpp](../../../../tests/integration/image/test_result_image_composite.cpp). See [NUM-08 Result Whole execution](../interpolation-whole.md) for current commands and evidence.

Strict execution implements the exact blend through the current Result Whole path. The accelerated profiles are registered under the three keys above; platform availability depends on the selected profile and host. The current strict and Apple Silicon oracle, root test, and installed consumer results are summarized in [NUM-08 Result Whole execution](../interpolation-whole.md). No x86 execution, native GPU support or performance result is claimed. The specification remains Proposed; implementation status describes code presence, not specification acceptance or cross-platform validation.
