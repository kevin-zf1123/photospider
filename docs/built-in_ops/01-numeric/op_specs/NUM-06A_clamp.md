---
spec_schema_version: 1
id: NUM-06A
parent_id: NUM-06
function: clamp
proposed_operation_keys:
  - numeric.clamp_strict
  - numeric.clamp_accelerated_apple_silicon
  - numeric.clamp_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-06A: clamp

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range and fallback rules. Clamp comparisons and selected results remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules
below and in the named family contract take precedence. This specification
remains Proposed; its status does not describe runtime registration.

## Tensor ports and Whole execution

The three input ports are `input`, `lower` and `upper`, in that order. Each is a
`Result` containing exactly one tensor member at index zero; its
`ResultTensorSpec::key` may be any key. The three members must have identical
`sample_shape()` and element type. The shape includes every input batch axis. Supported types are
UInt8, Int64, Float32 and Float64. There are no static numeric limit parameters,
implicit casts or implicit broadcasts.

The output port is `values`, a `Result` with schema `photospider.tensor` and
tensor key `samples`. It preserves the input element type and uses the complete
input `sample_shape()` as the output cell shape. Batch axes become ordinary
output axes, and output facets are empty. Input shape rank is 1..8 and its
complete logical element count is at most 2^40.

The three formal clamp profile keys use one Whole continuation. A nonempty
request issues one Need covering every sample on all three inputs with Data,
Validation and Descriptor roles (13). The continuation reads owning tensor
windows, validates every bound, then publishes the complete output through one
direct tensor writer. Sparse output demand does not narrow input validation or
the arithmetic domain. Any bound failure invalidates the complete invocation.
The output relation carries complete Data support from every input and retains
the full-input validation and descriptor obligations.

An Empty request publishes the declared output schema with empty sample
coverage. It issues no input payload Need and performs no sample arithmetic;
the callback does not run.

## Numeric behavior

At each global logical coordinate, validate `lower <= upper`. Equal bounds and
infinite floating-point bounds are valid. A NaN bound or `lower > upper` fails
the complete invocation, even if the coordinate lies outside the requested
output projection. An input NaN does not hide an invalid bound.

With valid bounds, quiet an input NaN while preserving its sign and payload.
Otherwise choose `lower` when `input < lower`, choose `upper` when
`input > upper`, and copy the input bits when it lies in range. Comparisons
treat signed zeros as equal, so an in-range `-0` remains `-0`; in particular,
`clamp(-0,+0,+0) = -0`. The operation is not defined by composing minimum and
maximum. Integer order uses exact signed comparison, and selected values are
not converted or rounded.

## Errors, resources and acceptance

Shape, dtype, rank or element-count mismatch fails metadata specialization as a
schema/type error. Typed semantic-coverage validation preserves its originating
`Status`, including `FailureReason::None` when that is the source status; it is
not relabeled as a numeric bounds error. Numeric bound failure returns
`InvalidArgument` with `FailureReason::InvalidDomain`, Domain origin, Run scope
and no Atom key. Its diagnostic identifies the bound port and raw bits and
includes the complete logical coordinate, including any former batch prefix.

The Whole callback owns one fixed `RangeMath` workspace through the phase
allocator and charges work to the execution root. Complete input windows,
output payload and workspace coexist during execution. Capacity, work-limit,
cancellation or numeric failure publishes no partial output and releases
unpublished scratch and payload. Empty requests avoid input payload allocation
and sample work.

The current public Result workflow and focused test are described in
[NUM-06 Result Whole execution](../range-whole.md). The workflow directly checks
Float32/Float64 paths, negative and zero-stride Int64 layouts, invalid bounds
outside sparse demand, typed validation, Whole support, Empty, resource
exhaustion, cache behavior and global-coordinate projection. The independent
oracle covers UInt8 and Int64 boundary values. The separate
[`test_numeric_result_math_sequences.cpp`](../../../../tests/integration/numeric/test_numeric_result_math_sequences.cpp)
fixture retains additional checks for all four dtypes, integer extrema,
batch-axis flattening, negative zero and pre-cancellation; it was not rerun for
this update.

Input values `[-1, 0.5, 2]`, lower `[0,0,0]` and upper `[1,1,1]` produce
`[0,0.5,1]`. The current [`ranges.cpp` workflow](../../../../examples/numeric_workflow/ranges.cpp)
binds tensor Results and reads the published `values` Result.

The code declares strict, Apple Silicon and x86-64 profile keys. The current
root Result test, Apple Silicon workflow, independent strict/Apple Fraction
oracle and installed consumer are summarized in [NUM-06 Result Whole
execution](../range-whole.md). No x86 execution, native GPU support or
performance result is claimed. These evidence boundaries do not change the
Proposed status.
