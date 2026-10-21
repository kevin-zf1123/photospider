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
---

# NUM-02B: arange

The strict key follows the exact reference below. Accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md), including its range and fallback rules; copies, endpoints and special values remain exact. Inherit the [NUM baseline](NUM_common_contract.md) for status, registration, shared execution and acceptance requirements. The explicit rules here take precedence.

`numeric.arange_{strict,accelerated_apple_silicon,accelerated_x86_64}` creates a one-dimensional sequence from `start`, `step` and a static `count`. Each input is a Result containing exactly one tensor member at any member key. Its complete `sample_shape()` must be `[1]`. `count` is a required static Int64 in `[1, 1048576]`; `dtype` is a required static String, `int64`, `float32` or `float64`.

Integer mode requires both inputs to be Int64 and produces Int64 values and an Int64 axis. Floating mode accepts Float32 or Float64 inputs, including mixed floating input types, and produces Float32 or Float64 values with a Float64 axis. Integer/float mixing requires an explicit cast. The public `arange_node` helper defaults to Int64 when both input schemas are Int64, otherwise Float64; callers can explicitly select Float32.

The `values` output is a Result with schema `photospider.tensor`, member `samples`, shape `[count]` and the selected dtype. The `axis` output uses that schema and member key with shape `[3]` and `atomic_trailing_axes=1`. Both outputs use ordinary tensor axes and drop batch topology. The axis tuple is `[start, last, step]`, with Int64 elements in integer mode and Float64 elements in floating mode. A request for one axis component closes demand over the full tuple. Values and axis are independent output identities.

## Exact progression and axis

For every global index `i`, define the exact value

```text
v_i = exact(start) + i * exact(step), 0 <= i < N
```

In integer mode, evaluate exactly; final `v_i` representability is a caller precondition. An intermediate product outside Int64 is not an error if the final sum fits. In floating mode, treat input values as exact binary rationals and round each `v_i` directly to the selected dtype using round-to-nearest, ties-to-even. Do not accumulate a rounded step across samples. Float32 output does not pass through an intermediate rounded Float64 sample.

For `N>=2`, integer axis is `[start, v_(N-1), step]`, with final Int64 representability required for the tuple. Floating axis is `[start, RN64(v_(N-1)), step]`; `last` is not narrowed to the Float32 value at the last sample. For `N=1`, integer values is the exact Int64 start; floating values is `[RN_dtype(start)]`. The axis is `[start, start, +0]`, without reading the step payload. Positive, negative and zero steps are valid. Rounded samples may repeat; the operation does not require distinct coordinates.

Dynamically read floating inputs and rounded computed components must be finite as unchecked caller preconditions. Underflow to subnormals or signed zero is allowed. Index zero preserves the sign of start's zero through conversion. At later indices, an exact zero is negative only when start and step are both negative zero; otherwise it is positive. Nonzero exact values rounded to zero retain their sign. The singleton axis uses positive zero in its last component.

## Fixed execution and numerical preconditions

Each selected values tile becomes one fixed CPU task with an independent dense allocation. Axis is one complete three-component task, selected independently. For count greater than one each task reads both scalar inputs, even for endpoint-only output. Count one reads only start and has a schema-only reference to the second input; the second schema remains fully checked. Empty selections emit no tasks or payload reads. Values use global indices, and unselected coordinates are not computed.

Finite active floating inputs, finite rounded selected floating outputs and representable selected Int64 outputs are unchecked caller preconditions. No numerical rejection or diagnostic is promised outside this domain. Unselected outputs impose no output-range requirement. Axis's complete tuple must satisfy the precondition whenever axis is selected. Underflow remains valid. This execution contract overrides the baseline's numerical validation and complete-input mechanisms.

Prepare validates both output schemas, canonical write views, exact active scalar reads, task parameters and scratch. Each invocation uses preallocated SequenceMath scratch and restores its floating environment. Selected tasks share one IR call: operational failure prevents successful publication of that call's outputs. There is no within-call output failure isolation. Static malformed parameters fail InvalidArgument and incompatible input/output schemas fail TypeMismatch. Resource, cancellation, backend and upstream failures retain their operational statuses.

Only selected values tiles are allocated; axis owns 24 bytes. Runtime creates no payload allocation and checks cancellation per sample. There are no numerical validators, per-sample work counters or content caches. Successful outputs retain storage beyond context destruction.

## Authoring and current checks

`numeric::SequenceInput` stores a workflow input reference and immutable single-tensor Result schema hint without retaining payload. `sequence_input` constructs it from a workflow input declaration. `arange_node` emits explicit `count` and `dtype`; its default dtype is Int64 for two Int64 schemas and Float64 otherwise. The helper preserves the explicit CPU profile key.

Temporary public-path checks use an independent Fraction/IEEE oracle and actual Compiler/ExecutionContext execution. Cover values-only, axis-only, joint and empty selection; count-one producer elimination; static rejection of an invalid excluded schema; direct Float32 rounding; signed zero; exact extreme cancellation; integer intermediate products when applicable; sparse requests; artifacts and concurrent Runs. Include `INT64_MIN + 2*INT64_MAX = INT64_MAX-1`: the intermediate product exceeds Int64 while the final sample remains representable. Test only configurations satisfying selected numerical preconditions. Source stride and floating-environment checks remain required.

The implementation uses 68 base-2^32 limbs and direct destination rounding for strict and available accelerated profiles. Unsupported accelerated targets return BackendUnavailable; no runtime profile substitution occurs. See [fixed sequence execution](../sequences-execution.md) for storage, call publication and validation scope. Specification status remains Proposed.
