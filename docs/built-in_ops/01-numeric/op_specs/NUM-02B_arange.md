---
spec_schema_version: 1
id: NUM-02B
parent_id: NUM-02
function: arange
operation_family: numeric.arange
proposed_operation_keys:
  - numeric.arange_strict
  - numeric.arange_accelerated_apple_silicon
  - numeric.arange_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
spec_revision: 0.2.0
document_maturity: D1_draft
implementation_status: implemented
repository_branch: ops-specs
repository_commit: current working tree
---

# NUM-02B: arange

The strict key follows the exact reference below. Accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md), including its range and fallback rules; copies, endpoints and special values remain exact. Inherit the [NUM baseline](NUM_common_contract.md) for status, registration, shared execution and acceptance requirements. The explicit rules here take precedence.

`numeric.arange_{strict,accelerated_apple_silicon,accelerated_x86_64}` creates a one-dimensional sequence from `start`, `step` and a static `count`. Each input is a Result containing exactly one tensor member at any member key. Its complete `sample_shape()` must be `[1]`. `count` is a required static Int64 in `[1, 1048576]`; `dtype` is a required static String, `int64`, `float32` or `float64`.

Integer mode requires both inputs to be Int64 and produces Int64 values and an Int64 axis. Floating mode accepts Float32 or Float64 inputs, including mixed floating input types, and produces Float32 or Float64 values with a Float64 axis. Integer/float mixing requires an explicit cast. The public `arange_node` helper defaults to Int64 when both input schemas are Int64, otherwise Float64; callers can explicitly select Float32.

The `values` output is a Result with schema `photospider.tensor`, member `samples`, shape `[count]` and the selected dtype. The `axis` output uses that schema and member key with shape `[3]` and `atomic_trailing_axes=1`. Both outputs use ordinary tensor axes and drop facets and batch topology. The axis tuple is `[start, last, step]`, with Int64 elements in integer mode and Float64 elements in floating mode. A request for one axis component closes demand over the full tuple. Values and axis are independent output identities.

## Exact progression and axis

For every global index `i`, define the exact value

```text
v_i = exact(start) + i * exact(step), 0 <= i < N
```

In integer mode, evaluate exactly and check only the final `v_i` against Int64 bounds. An intermediate product outside Int64 is not an error if the final sum fits. In floating mode, treat input values as exact binary rationals and round each `v_i` directly to the selected dtype using round-to-nearest, ties-to-even. Do not accumulate a rounded step across samples. Float32 output does not pass through an intermediate rounded Float64 sample.

For `N>=2`, integer axis is `[start, v_(N-1), step]`, with an independent Int64 range check for the final value. Floating axis is `[start, RN64(v_(N-1)), step]`; `last` is not narrowed to the Float32 value at the last sample. For `N=1`, integer values is the exact Int64 start; floating values is `[RN_dtype(start)]`. The axis is `[start, start, +0]`, without reading the step payload. Positive, negative and zero steps are valid. Rounded samples may repeat; the operation does not require distinct coordinates.

Only dynamically read floating inputs must be finite, and every published component must be finite. Underflow to subnormals or signed zero is allowed. Index zero preserves the sign of start's zero through conversion. At later indices, an exact zero is negative only when start and step are both negative zero; otherwise it is positive. Nonzero exact values rounded to zero retain their sign. The singleton axis uses positive zero in its last component.

## Result demand and failure behavior

Both outputs use Whole execution. A nonempty request reads authorized scalar input windows and computes the complete selected output through a packed Result writer before consumer projection. The published Result retains the complete object and its global coordinates. For `count > 1`, it requests both inputs with Data, Validation and Descriptor roles (role 13), even when the consumer selects only an endpoint. For `count=1`, static specialization selects only input 0 for runtime Need and relation support; complete static metadata for the other input remains subject to specialization and seal checks. Empty requests read no payload and perform no arithmetic.

Static preflight rejects missing or malformed parameters with `InvalidArgument`; unsupported input dtype, nonscalar input shape or a mismatch between selected integer/floating mode and input kinds uses `TypeMismatch`. An Int64 sample overflow fails the selected `values` request with `OperationFailed` / `ArithmeticOverflow` and identifies the global index. A floating sample that rounds to infinity fails the same output. An axis overflow fails only the selected axis observation; it does not revoke a separately successful values result. Nonfinite read inputs fail with `OperationFailed` / `InvalidDomain`. Runtime numeric failures are Domain/Run failures. Upstream, typed-validation, resource, cancellation and stale-input failures retain their original status. No partial selected output is published.

Active input edits invalidate the complete selected output. Values own `count * sizeof(dtype)` bytes; axis owns 24 bytes. Exact arithmetic workspace is admitted through the phase allocator. Work, resource limits and cancellation are charged during generation; failure releases unpublished output and scratch. These payload sizes are not an RSS bound.

## Authoring and current checks

`numeric::SequenceInput` stores a workflow input reference and immutable single-tensor Result schema hint without retaining payload. `sequence_input` constructs it from a workflow input declaration. `arange_node` emits explicit `count` and `dtype`; its default dtype is Int64 for two Int64 schemas and Float64 otherwise. The helper preserves the explicit CPU profile key.

The focused Result sequence fixture runs through:

```sh
ctest --test-dir build/kernel-dev -R '^test_numeric_sequences_result$' --output-on-failure
```

The workflow loop checks strict and available Apple Silicon execution for Float64 sequences. Separate strict cases check Int64 values beyond 2^53, exact intermediate-product cancellation with a representable final sum, and axis and values selection. For example, `INT64_MIN + 2*INT64_MAX` must produce `INT64_MAX-1`; the intermediate product is outside Int64, but the exact final value fits. Shared sequence fixtures check negative zero, Empty, pre-cancellation, work-limit rollback, public Result-only authoring through a runnable workflow, direct Float32 rounding and whole-output overflow, count-one producer exclusion with static descriptor rejection, and count-two upstream failure. These shared cases do not establish every arange dtype/profile combination. The x86 unavailable path is checked, not x86 execution. The independent Fraction/IEEE oracle and installed SDK consumer are described in [sequences Whole](../sequences-whole.md); neither supplies an x86 execution or performance claim.

The current implementation uses fixed-width exact dyadic state for integer and floating modes and direct rounding in its accelerated paths as well as strict. This exact implementation is an implementation fact; the public accelerated floating accuracy promise remains the shared FP32 four-ULP contract. An unsupported accelerated target returns `BackendUnavailable`; the runtime does not substitute a different key. No NUM-14 certificate is applied. See [sequences Whole](../sequences-whole.md) for the shared execution and test boundaries.
