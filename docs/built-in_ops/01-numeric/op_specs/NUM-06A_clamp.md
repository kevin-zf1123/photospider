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

## Tensor ports and fixed execution

The three input ports are `input`, `lower` and `upper`, in that order. Each is a
`Result` containing exactly one tensor member at index zero; its
`ResultTensorSpec::key` may be any key. The three members must have identical
`sample_shape()` and element type. The shape includes every input batch axis. Supported types are
UInt8, Int64, Float32 and Float64. There are no static numeric limit parameters,
implicit casts or implicit broadcasts.

The output port is `values`, a `Result` with schema `photospider.tensor` and
tensor key `samples`. It preserves the input element type and uses the complete
input `sample_shape()` as the output cell shape. Batch axes become ordinary
output axes, and output carries no domain description. Input shape rank is 1..8 and its
complete logical element count is at most 2^40.

Each requested output tile lowers to one fixed CPU task with the intersecting tiles of all three input ports. Lowering uses each input's own batch, spatial and atomic-axis grid. Prepare checks the exact ordered read slots, per-port dtype, complete regions, output view, kernel contract and scratch reservation. Each output tile has a separate preallocated dense allocation.

Sparse selection computes only selected output tiles. Empty selection emits no tasks or payload reads and returns the declared schema with empty coverage. All declared input owners remain dependencies through completion.

## Numeric behavior

The caller supplies non-NaN bounds satisfying `lower <= upper` at each computed coordinate. Equal bounds and infinite floating bounds are valid. These are unchecked numerical preconditions; no error outcome is promised for invalid bounds.

With valid bounds, quiet an input NaN while preserving its sign and payload.
Otherwise choose `lower` when `input < lower`, choose `upper` when
`input > upper`, and copy the input bits when it lies in range. Comparisons
treat signed zeros as equal, so an in-range `-0` remains `-0`; in particular,
`clamp(-0,+0,+0) = -0`. The operation is not defined by composing minimum and
maximum. Integer order uses exact signed comparison, and selected values are
not converted or rounded.

## Errors, resources and acceptance

Metadata specialization rejects incompatible dtype, sample shape, rank or element count before issue. Input binding and task signature checks are structural. Numerical bound validity belongs to the caller. Resource, upstream and cancellation failures retain their operational status codes.

The Plan declares `sizeof(RangeMath)` Host scratch per task; block admission reserves distinct scratch for each active worker lane before issue. Prepared state is immutable across Runs. Invocation computes into preallocated selected output tiles without payload allocation. Arithmetic checkpoints query cancellation per sample and during exact refinement; they do not debit a dynamic sample-work budget. Failure discards unpublished writes after accepted callbacks return. Successful Result owners survive the context.

Temporary public acceptance should compare exact integer/bit selections and independent Fraction remap references. Cover supported dtypes, integer extrema, signed strides, different input grids, nonzero tile origins, sparse/empty requests, pre-cancellation, imported Plans, malformed signatures and concurrent Runs. For remap, include reversed/equal target bounds, endpoint zero signs, extreme finite source widths and extrapolation. See [NUM-06 fixed range execution](../range-execution.md).

Input values `[-1, 0.5, 2]`, lower `[0,0,0]` and upper `[1,1,1]` produce
`[0,0.5,1]`.

The code declares strict, Apple Silicon and x86-64 profile keys. Execution behavior and temporary acceptance cases are described in
[NUM-06 fixed range execution](../range-execution.md). Validate each admitted profile
against independent Fraction references. The specification remains Proposed;
its cases do not report current hardware or performance results.
