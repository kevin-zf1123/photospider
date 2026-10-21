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

## Tensor ports and fixed execution

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
output axes, and output carries no domain description. Input shape rank is 1..8 and its
complete logical element count is at most 2^40.

Each requested output tile lowers to one fixed CPU task with the intersecting tiles of all five input ports. Lowering uses each input's own batch, spatial and atomic-axis grid. Prepare checks the exact ordered read slots, per-port dtype, complete regions, output view, kernel contract and scratch reservation. Each output tile has a separate preallocated dense allocation.

Sparse selection computes only selected output tiles. Empty selection emits no tasks or payload reads and returns the declared schema with empty coverage. All declared input owners remain dependencies through completion.

## Numeric behavior

For finite ordinary inputs, the mathematical formula is

$$
y = t_0 + (x-s_0)\,\frac{t_1-t_0}{s_1-s_0}.
$$

The caller supplies four finite bounds with `source_lower < source_upper`. Target bounds may increase, decrease or be equal. These are unchecked numerical preconditions; no error outcome is promised for invalid bounds.

An input NaN is quieted while its sign and payload are
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

Metadata specialization rejects incompatible dtype, sample shape, rank or element count before issue. Input binding and task signature checks are structural. Numerical bound validity belongs to the caller. Resource, upstream and cancellation failures retain their operational status codes.

The Plan declares `sizeof(RangeMath)` Host scratch per task; block admission reserves distinct scratch for each active worker lane before issue. Prepared state is immutable across Runs. Invocation computes into preallocated selected output tiles without payload allocation. Arithmetic checkpoints query cancellation per sample and during exact refinement; they do not debit a dynamic sample-work budget. Failure discards unpublished writes after accepted callbacks return. Successful Result owners survive the context.

Temporary public acceptance should compare exact integer/bit selections and independent Fraction remap references. Cover supported dtypes, integer extrema, signed strides, different input grids, nonzero tile origins, sparse/empty requests, pre-cancellation, imported Plans, malformed signatures and concurrent Runs. For remap, include reversed/equal target bounds, endpoint zero signs, extreme finite source widths and extrapolation. See [NUM-06 fixed range execution](../range-execution.md).

For example, Float64 input `[0,0.5,1,2]` with source bounds `[0,1]` and target
bounds `[0,255]` explicitly broadcast to the input shape produces
`[0,127.5,255,510]`.

The code declares strict, Apple Silicon and x86-64 profile keys. Execution behavior and temporary acceptance cases are described in
[NUM-06 fixed range execution](../range-execution.md). Validate each admitted profile
against independent Fraction references. The specification remains Proposed;
its cases do not report current hardware or performance results.
