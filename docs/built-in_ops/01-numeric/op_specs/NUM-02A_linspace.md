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
---

# NUM-02A: linspace

The strict key follows the exact reference below. Accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md), including its range and fallback rules; copies, endpoints and special values remain exact. Inherit the [NUM baseline](NUM_common_contract.md) for status, registration, shared execution and acceptance requirements. The explicit rules here take precedence.

`numeric.linspace_{strict,accelerated_apple_silicon,accelerated_x86_64}` creates a one-dimensional sequence from `start`, `end` and a static `count`. Each input is a Result containing exactly one tensor member at any member key. Its complete `sample_shape()` must be `[1]`; both inputs are Float32 or Float64 and may use different floating types. `count` is a required static Int64 in `[1, 1048576]`. `dtype` is a required static String, `float32` or `float64`. The public `linspace_node` helper defaults to Float64 output and the strict CPU profile; callers can select another supported CPU profile explicitly.

The `values` output is a Result with schema `photospider.tensor`, member `samples`, shape `[count]` and the selected floating dtype. The `axis` output uses the same schema and member key, Float64 shape `[3]`, and `atomic_trailing_axes=1`. Both outputs use ordinary tensor axes and drop batch topology. The axis tuple is `[start, end, step]`. Values and axis are independent output identities; requesting an axis component closes demand over the complete three-component tuple.

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

Dynamically read floating inputs and rounded computed components must be finite as unchecked caller preconditions. A selected axis requires a finite rounded step; selected values tiles require finite rounded samples only within those tiles. Subnormals and gradual underflow to signed zero are valid. The implementation restores floating rounding modes and exception flags.

## Fixed execution and numerical preconditions

Each selected values tile becomes one fixed CPU task with an independent dense allocation. Axis is one complete three-component task, selected independently. For count greater than one each task reads both scalar inputs, even for endpoint-only output. Count one reads only start and has a schema-only reference to the second input; the second schema remains fully checked. Empty selections emit no tasks or payload reads. Values use global indices, and unselected coordinates are not computed.

Finite active floating inputs and finite rounded selected floating outputs are unchecked caller preconditions. No numerical rejection or diagnostic is promised outside this domain. Unselected outputs impose no output-range requirement. Axis's complete tuple must satisfy the precondition whenever axis is selected. Underflow remains valid. This execution contract overrides the baseline's numerical validation and complete-input mechanisms.

Prepare validates both output schemas, canonical write views, exact active scalar reads, task parameters and scratch. Each invocation uses preallocated SequenceMath scratch and restores its floating environment. Selected tasks share one IR call: operational failure prevents successful publication of that call's outputs. There is no within-call output failure isolation. Static malformed parameters fail InvalidArgument and incompatible input/output schemas fail TypeMismatch. Resource, cancellation, backend and upstream failures retain their operational statuses.

Only selected values tiles are allocated; axis owns 24 bytes. Runtime creates no payload allocation and checks cancellation per sample. There are no numerical validators, per-sample work counters or content caches. Successful outputs retain storage beyond context destruction.

## Authoring and current checks

`numeric::SequenceInput` carries a workflow input reference and an immutable single-tensor Result schema hint; it contains no payload. `sequence_input` builds it from a workflow input declaration. `linspace_node` writes explicit `count` and `dtype` parameters and defaults to Float64. Changing either parameter requires recompilation.

Temporary public-path checks use an independent Fraction/IEEE oracle and actual Compiler/ExecutionContext execution. Cover values-only, axis-only, joint and empty selection; count-one producer elimination; static rejection of an invalid excluded schema; direct Float32 rounding; signed zero; exact extreme cancellation; integer intermediate products when applicable; sparse requests; artifacts and concurrent Runs. Include `start=1`, `end=0x1.0000020000001p0`, `count=3`, Float32 linspace: the middle value must be `0x3f800001`, avoiding Float64 intermediate double rounding. Test only configurations satisfying selected numerical preconditions. Source stride and floating-environment checks remain required.

The implementation uses 68 base-2^32 limbs and direct destination rounding for strict and available accelerated profiles. Unsupported accelerated targets return BackendUnavailable; no runtime profile substitution occurs. See [fixed sequence execution](../sequences-execution.md) for storage, call publication and validation scope. Specification status remains Proposed.
