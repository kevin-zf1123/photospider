---
spec_schema_version: 1
id: NUM-06B
parent_id: NUM-06
function: remap_range
proposed_operation_keys:
  - numeric.remap_range_strict
  - numeric.remap_range_accelerated_apple_silicon
  - numeric.remap_range_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: current working tree
---

# NUM-06B: remap_range

Map a source interval to a target interval and extrapolate linearly outside the
source interval. Strict uses the exact reference formula. Floating arithmetic
in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
verified enclosure and fallback rules.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules
below and in the named family contract take precedence. This specification
remains Proposed; its status does not describe runtime registration.

## Tensor ports and Whole execution

Inputs are five `Result` tensor members, in port order: `input`,
`source_lower`, `source_upper`, `target_lower`, `target_upper`. Each input
`Result` contains exactly one tensor member at tensor index zero, with any
`ResultTensorSpec::key`. All five must have
identical `sample_shape()` and element type, Float32 or Float64. The shape
includes every declared batch axis. There are no static numeric parameters,
implicit casts or implicit broadcasts. Clipping is a separate clamp operation.

The output port is `values`, a `Result` with schema `photospider.tensor` and
tensor key `samples`. It preserves the input element type and uses the complete
input `sample_shape()` as the output cell shape. Batch axes become ordinary
output axes, and output facets are empty. Input shape rank is 1..8 and its
complete logical element count is at most 2^40.

The three formal remap profile keys use one Whole continuation. A nonempty
request issues one Need covering every sample on all five inputs with Data,
Validation and Descriptor roles (13). The continuation reads owning tensor
windows, validates all four bounds, then publishes the complete output through
one direct tensor writer. Sparse output demand does not narrow validation or
arithmetic. Any invalid bound fails the complete invocation, even when the
invalid coordinate is outside the requested output region. The output relation
carries complete Data support from every input and retains full-input
Validation and Descriptor obligations.

An Empty request publishes the declared output schema with empty sample
coverage. It issues no input payload Need and performs no sample arithmetic;
the callback does not run.

## Numeric behavior

For finite ordinary inputs, the mathematical formula is

$$
y = t_0 + (x-s_0)\,\frac{t_1-t_0}{s_1-s_0}.
$$

All four bounds must be finite and `source_lower < source_upper`. A NaN or
infinite bound, or a non-increasing source interval, is invalid. Target bounds
may increase, decrease or be equal. Bound validation precedes input-NaN
handling, so an input NaN never hides an invalid bound.

After bounds validate, an input NaN is quieted while its sign and payload are
preserved. Apply the remaining cases in this order:

1. Equal target bounds return the `target_lower` bits for every non-NaN input,
   including infinite inputs and opposite-sign target zeros.
2. An exact `source_lower` input returns the `target_lower` bits.
3. An exact `source_upper` input returns the `target_upper` bits.
4. An infinite input maps to the correctly signed infinity according to the
   exact slope sign.
5. A finite input evaluates the whole formula as an exact dyadic rational and
   rounds once to the output type.

A finite mathematical result that overflows returns the correctly signed
infinity. Gradual underflow preserves the exact result sign; an exact
non-special zero is positive zero. Endpoint selection preserves the supplied
target endpoint bits.

The strict profile rounds the exact rational result directly. Accelerated
profiles may use the shared hardware quotient only when its final-error
enclosure passes the [FP32 four-ULP contract](NUM_accelerated_contract.md);
otherwise they use strict exact rounding. No NUM-14 certificate or weaker
approximation is introduced.

## Errors, resources and acceptance

Shape, dtype, rank or element-count mismatch fails metadata specialization as a
schema/type error. Typed semantic-coverage validation preserves its originating
`Status`, including `FailureReason::None` when that is the source status; it is
not relabeled as a numeric bounds error. Numeric bound failure returns
`InvalidArgument` with `FailureReason::InvalidDomain`, Domain origin, Run scope
and no Atom key. Its diagnostic identifies the invalid bound port and raw bits
and includes the complete logical coordinate, including any former batch
prefix.

The Whole callback owns a fixed `RangeMath` workspace through the phase
allocator and charges work to the execution root. All five complete input
windows, complete output payload and workspace coexist during execution. Exact
rational work can exceed the root work limit. Capacity, work-limit,
cancellation or numeric failure publishes no partial output and releases
unpublished scratch and payload. Empty requests avoid input payload allocation
and sample work.

The current public Result workflow and focused test are described in
[NUM-06 Result Whole execution](../range-whole.md). The workflow checks
remap-to-clamp composition, invalid bounds outside sparse demand, all five input
obligations, Empty, Whole support, typed validation through clamp, cache
invalidation, and rational work exhaustion. The strict and Apple Silicon
oracles cover the remap formulas and boundary values. The separate
[`test_numeric_result_math.cpp`](../../../../tests/integration/test_numeric_result_math.cpp)
fixture retains checks for an overflowing intermediate subtraction, invalid
bounds despite input NaN, batch-axis flattening and pre-cancellation; it was not
rerun for this update.

For example, Float64 input `[0,0.5,1,2]` with source bounds `[0,1]` and target
bounds `[0,255]` explicitly broadcast to the input shape produces
`[0,127.5,255,510]`. The current [`ranges.cpp` workflow](../../../../examples/numeric_workflow/ranges.cpp)
binds tensor Results and reads the published `values` Result.

The code declares strict, Apple Silicon and x86-64 profile keys. The current
root Result test, Apple Silicon workflow, independent strict/Apple Fraction
oracle and installed consumer are summarized in [NUM-06 Result Whole
execution](../range-whole.md). No x86 execution, native GPU support or
performance result is claimed. These evidence boundaries do not change the
Proposed status.
