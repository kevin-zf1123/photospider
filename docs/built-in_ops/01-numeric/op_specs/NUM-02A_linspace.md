---
spec_schema_version: 1
id: NUM-02A
parent_id: NUM-02
function: linspace
operation_family: numeric.linspace
proposed_operation_keys:
  - numeric.linspace_strict
  - numeric.linspace_accelerated_apple_silicon
  - numeric.linspace_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
spec_revision: 0.2.0
document_maturity: D1_draft
implementation_status: implemented
repository_branch: ops-specs
repository_commit: current working tree
---

# NUM-02A: linspace

The strict key follows the exact reference below. Accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md), including its range and fallback rules; copies, endpoints and special values remain exact. Inherit the [NUM baseline](NUM_common_contract.md) for status, registration, shared execution and acceptance requirements. The explicit rules here take precedence.

`numeric.linspace_{strict,accelerated_apple_silicon,accelerated_x86_64}` creates a one-dimensional sequence from `start`, `end` and a static `count`. Each input is a Result containing exactly one tensor member at any member key. Its complete `sample_shape()` must be `[1]`; both inputs are Float32 or Float64 and may use different floating types. `count` is a required static Int64 in `[1, 1048576]`. `dtype` is a required static String, `float32` or `float64`. The public `linspace_node` helper defaults to Float64 output and the strict CPU profile; callers can select another supported CPU profile explicitly.

The `values` output is a Result with schema `photospider.tensor`, member `samples`, shape `[count]` and the selected floating dtype. The `axis` output uses the same schema and member key, Float64 shape `[3]`, and `atomic_trailing_axes=1`. Both outputs use ordinary tensor axes and drop facets and batch topology. The axis tuple is `[start, end, step]`. Values and axis are independent output identities; requesting an axis component closes demand over the complete three-component tuple.

## Exact sequence and axis

For `N=count >= 2`, interpret the finite input endpoints as exact binary rational values and define each sample from its global index:

```text
values[0]   = RN_dtype(start)
values[N-1] = RN_dtype(end)
values[i]   = RN_dtype(((N-1-i)*exact(start) + i*exact(end))/(N-1)), 0<i<N-1
```

`RN_dtype` means direct round-to-nearest, ties-to-even in the selected output type. Float32 output never passes through an already-rounded interpolated Float64 value. The kernel computes each sample from its index rather than repeatedly adding a rounded step. For `N=1`, values is `[RN_dtype(start)]` and axis is `[start, start, +0]`; runtime uses only the start payload.

For `N>=2`, axis is `[start, end, RN64((exact(end)-exact(start))/(N-1))]`. The difference and quotient are evaluated without an overflowing Float64 intermediate. Equal endpoints are valid and produce a positive-zero step. A nonzero exact step may round to signed zero. Axis retains widened endpoint values even when values use Float32. Reconstruct samples from endpoints and count; repeatedly adding the rounded step is not the sequence definition.

Equal endpoints and duplicate rounded samples are valid. Consumers that require strictly ordered coordinates must check that condition themselves. Endpoints preserve their signed-zero bits through conversion. An interior exact zero is negative only when both endpoints are negative zero; otherwise it is positive zero. A nonzero exact result rounded to zero retains its sign. A constant negative-zero sequence therefore contains negative-zero values, while its axis step is positive zero.

Only dynamically read floating inputs must be finite, and every published component must be finite. A step rounding overflow fails the selected `axis` output. Any sample overflow fails a nonempty `values` request, including when the overflowing global index lies outside the consumer projection. Subnormals and gradual underflow to signed zero are valid. The implementation restores the caller's rounding mode and exception flags.

## Result demand and failure behavior

Both outputs use Whole execution. For a nonempty request, the program reads authorized input tensor windows and computes the complete selected output through a packed Result writer before consumer projection. The published Result retains the complete object and its global coordinates. For `count > 1`, it requests both scalar inputs with Data, Validation and Descriptor roles (role 13). Thus an endpoint-only values request still depends on both inputs, and a failed end producer can fail the request. For `count=1`, static specialization selects only input 0 for runtime Need and relation support for either output. The second input's complete static metadata is still checked during specialization and seal; its payload and producer are not requested. Empty requests do not read payload or perform arithmetic.

Static preflight rejects missing or malformed parameters with `InvalidArgument`; unsupported input dtype, nonscalar input shape or a non-floating output dtype uses `TypeMismatch`. Any active source change invalidates the complete selected output. An axis failure leaves a separately successful values result intact. Runtime nonfinite inputs fail with `OperationFailed` / `InvalidDomain`; rounded output overflow uses `OperationFailed` / `ArithmeticOverflow`. Both are Domain/Run failures identified with the selected output and global sample or axis component. Resource, cancellation, stale-input, typed-validation and upstream failures retain their original status. Failure publishes no partial output.

Output values own `count * sizeof(dtype)` payload bytes. Axis owns 24 bytes. The `SequenceMath` workspace is admitted through the phase allocator. The host resource ledger accounts for input windows, output and scratch; work and cancellation are checked during generation and before publication. Failure releases unpublished output and scratch. These payload sizes are not an RSS bound.

## Authoring and current checks

`numeric::SequenceInput` carries a workflow input reference and an immutable single-tensor Result schema hint; it contains no payload. `sequence_input` builds it from a workflow input declaration. `linspace_node` writes explicit `count` and `dtype` parameters and defaults to Float64. Changing either parameter requires recompilation.

The focused Result sequence fixture runs through:

```sh
ctest --test-dir build/kernel-dev -R '^test_numeric_sequences_result$' --output-on-failure
```

The workflow loop checks strict and available Apple Silicon execution with Float64 endpoints. Separate strict cases check direct Float32 rounding with `start=1`, `end=0x1.0000020000001p0`, and `count=3`: the middle result is `0x3f800001`, while Float64-then-Float32 double rounding gives `0x3f800000`. Other checks cover endpoint and axis output selection, atomic tuple closure, extreme finite cancellation, signed zero, axis-only overflow, failure from an overflowing value outside requested coverage, nonfinite-input failure, Empty, work-limit rollback and pre-cancellation. It also checks count-one exclusion of a failing end producer while retaining static dtype validation, plus count-two upstream failure. The x86 profile's unavailable path is checked; this is not an x86 execution result. The focused workflow is supplemented by the independent Fraction/IEEE oracle and installed SDK consumer described in [sequences Whole](../sequences-whole.md); neither supplies an x86 execution or performance claim.

The current implementation uses 68 base-2^32 limbs for finite binary64 rational arithmetic and direct IEEE rounding for its accelerated paths as well as strict. This exact implementation is an implementation fact; the public accelerated accuracy promise remains the shared FP32 four-ULP contract. An unsupported accelerated target returns `BackendUnavailable`; the runtime does not substitute a different key. No NUM-14 finite-Float32 certificate is used. Current shared storage and Whole behavior is summarized in [sequences Whole](../sequences-whole.md).
