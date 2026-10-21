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

The output port is `values`, a Result with schema `photospider.tensor` and tensor member key `samples`. Its dtype and complete shape match the input members. Every output axis is an ordinary axis; output batch axes are empty. The operation does not apply implicit broadcasting, casts, color-space conversion, alpha association, or image compositing. Connect a common scalar factor through an explicit broadcast operation.

## Numeric contract

For each logical coordinate, the mathematical blend is

$$
\operatorname{mix}(a,b,t)=(1-t)a+tb.
$$

The caller supplies finite factors satisfying `0 <= t <= 1`; negative zero is valid. This is an unchecked numerical precondition. The kernel does not validate factors or promise a diagnostic for invalid samples.

At `t = 0`, copy `a`'s bits; at `t = 1`, copy `b`'s bits. These selections preserve signaling-NaN and signed-zero encodings without floating-point evaluation. The Plan declares the intersecting tiles of all three inputs before execution, including both endpoints. Upstream operational failure on either declared input can therefore prevent execution.

For `0 < t < 1`, both endpoint samples participate. If either endpoint is NaN, return the first NaN in input order, quieting it while preserving sign and payload. If an endpoint is infinite, return that infinity when it is the only infinity or both infinities have the same sign; opposite-sign infinities produce the fixed positive quiet NaN. These cases produce numeric output successfully.

For finite endpoints and an interior factor, round the exact real expression directly once to the output dtype. Strict results are bitwise reproducible. Accelerated floating results use the shared FP32-scaled error bound. Two negative-zero endpoints produce negative zero; any other exact zero interior result produces positive zero. A nonzero exact result that underflows to zero keeps its mathematical sign. Since `t` is in [0,1], finite endpoints cannot produce an exact result outside their range. The implementation must avoid intermediate subtraction or products that introduce spurious overflow.

## Fixed invocation execution and resources

Each requested output tile lowers to one CPU task. The task reads every input tile intersecting that output region, using each input's own batch, spatial and atomic-axis grid. It writes one separate preallocated dense output allocation in global coordinates. Empty selection emits no task and requires no input payload. Sparse requests compute only selected output tiles.

Prepare validates the exact read slots, output region, kernel contract and scratch capacity before binding invocation ABI 8. The prepared context is immutable and may be shared by independent Runs. Each active worker lane receives a separate fixed interpolation workspace reserved before issue. Kernels use nearest rounding with gradual underflow, restore the worker floating environment, and query cancellation per sample and during exact arithmetic. Failure discards unpublished output; accepted invocations retain input, output and scratch owners until return. Successful Result owners survive the execution context.

Static metadata validation checks dtype, complete sample shape, rank and the 2^40 sample bound. Numerical preconditions belong to the caller. Resource, upstream and cancellation failures retain their operational status codes. See [NUM-08 fixed interpolation execution](../interpolation-execution.md).

## Acceptance and implementation evidence

The three profile keys implement the same fixed tile topology and their specified numerical rules. Profile availability is checked before execution. Temporary acceptance should compare strict results with independent exact arithmetic and check accelerated errors against the stated bound. Include nonzero tile origins, distinct input grids, empty selection, endpoint raw-bit copies, quieted NaNs, infinities, cancellation and retained output lifetime. The specification remains Proposed; implementation status does not establish cross-platform execution or performance.
