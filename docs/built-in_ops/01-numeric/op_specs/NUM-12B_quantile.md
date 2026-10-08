---
spec_schema_version: 1
id: NUM-12B
parent_id: NUM-12
function: quantile
proposed_operation_keys:
  - numeric.quantile_strict
  - numeric.quantile_accelerated_apple_silicon
  - numeric.quantile_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: current working tree
---

# NUM-12B: quantile

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration and shared execution rules. This member specifies one linear
quantile per logical line. Its specification status remains Proposed; that
status does not imply that the keys are absent from the runtime.

## Tensor ports, parameters and output

The `input` port is a `Result` containing exactly one tensor member at index
zero; its `ResultTensorSpec::key` may be any key. Supported source types are
UInt8, Int64, Float32 and Float64. The source `sample_shape()` includes declared
batch axes. The `q` port is another `Result` with exactly one tensor member at
index zero and any member key. Its `sample_shape()` is exactly `[1]` and its
element type is Float32 or Float64.

`axis` is a required static Int64 parameter in `0..rank-1`, measured against the
full source sample shape. `dtype` is a required static String parameter whose
value is `float32` or `float64`. These parameters and the q tensor schema are
validated as static metadata. The numerical q constraint, finite and in
`[0,1]`, is checked inside the callback for active q input; it is not a scalar
bounds preflight.

The `values` output is a `Result` with schema `photospider.tensor` and tensor key
`samples`. It has the selected output dtype and keeps the source rank and shape,
with the selected axis extent set to 1. Batch axes become ordinary output axes;
output batch topology and facets are empty. The full source rank is 1..8 and its
logical element count is at most 2^40.

## Rank and interpolation

For an active line of length N, put its samples in stable ascending numerical
order `x[0..N-1]`. Decode q as an exact binary rational and form

$$
h=(N-1)q,\qquad j=\lfloor h\rfloor,\qquad w=h-j.
$$

If `w == 0`, select `x[j]`. Otherwise calculate

$$
y=(1-w)x[j]+wx[j+1].
$$

This handles q=1 without reading beyond `x[N-1]`. The decoded significand has at
most 53 bits and N<=2^40, so `(N-1)*significand` fits in 93 bits; the integer
rank and exact remainder over a power-of-two denominator represent h. Integer
samples retain exact integer values through ordering and interpolation setup;
they are not first converted to floating point. The exact finite selection or interpolation is
rounded once to the output dtype. Strict uses exact final rounding.
Accelerated profiles use the shared [FP32 four-ULP contract](NUM_accelerated_contract.md)
and its verified quotient/fallback rules. No approximate quantile sketch,
rank-error tolerance or NUM-14 certificate is used.

## NaNs, infinities and zero signs

The complete active source line is ordered and checked for NaNs before selecting
the rank, including for q=0 or q=1. If a line contains a NaN, return the first
NaN in original axis order, quieted and converted to `dtype` under the
[reduction NaN payload rule](NUM-11_reduction_contract.md). Stable sorting keeps
NaNs after non-NaNs and preserves their original order. A source NaN does not
hide invalid q once the callback is reached.

When `w == 0`, convert the selected sample directly. This preserves its signed
zero and correctly converts a selected infinity. For `0 < w < 1`, two
same-sign infinities or one infinity paired with a finite value yield that
signed infinity. Opposite-sign infinities yield the fixed positive quiet NaN.
Two negative-zero interpolation endpoints produce negative zero; all other
exact zero mixtures produce positive zero. Nonzero exact results that underflow
retain their mathematical sign. Infinities outside the selected pair do not by
themselves produce NaN.

## Whole demand and input projection

For N>=2, the continuation issues a Need for the complete source and complete
q member with Data, Validation and Descriptor roles (13). It reads their
authorized Result windows and computes one complete keep-dimension output for
every line. Sparse consumer demand does not reduce the source or q domain. A
source edit or q edit invalidates all observed output coordinates. A source
producer or typed-input failure can occur before the callback checks q's
numeric domain.

For N==1, static metadata specialization validates q's shape and element type,
and validates `axis` and `dtype`, but projects runtime selection to source port
zero. The continuation issues no q payload Need and records no q runtime data,
validation or descriptor relation. A failing q producer therefore does not
participate in that request; changing q cannot invalidate its output.

An Empty request issues no payload Need, performs no line ordering or
interpolation, and publishes the declared output schema with empty sample
coverage.

## State, work and error behavior

The continuation allocates fixed `OrderingState` through the phase allocator's
Payload capacity. It uses the same per-line key and permutation metadata as
[sort](NUM-12A_sort.md): 16*L element bytes plus allocator headers, alignment
and Entries reservations per line, charged as Root metadata. Each line is
permuted once per callback. The exact position and interpolation workspace
lives in the fixed state. Authorized source and q windows coexist with the
complete output payload; sparse consumers still pay for the selected full
output. The operation does not first pack its complete inputs. There is no
permutation block cache shared across lines or independent invocations.

Work, reads, comparisons and exact arithmetic are charged to the execution
Root. Cancellation, work or capacity failure aborts the transactional Whole
writer, releases unpublished state and output, and preserves the first typed
failure. Invalid active q returns `InvalidArgument` with
`FailureReason::InvalidDomain`, Domain origin, Run scope and
`InvalidQuantileProbability` diagnostics containing the q bits. Invalid q is
not an Atom-scoped sample result.

## Current behavior checks

The current public behavior entry is `examples/numeric_workflow/ordering.cpp`,
registered as `test_numeric_ordering_result`; the same source is built as the
installed consumer `installed_numeric_ordering_result`. It checks the 7.5
landmark, q-schema validation, axis-length-one q exclusion despite a failing q
producer, source-failure precedence, source/q replacement, and output lifetime.
The existing `test_numeric_result_math.cpp` integration fixture contains
additional quantile cases, but it was not rerun for this Result migration. See
[NUM-12 Result Whole execution](../ordering-whole.md) for the current commands
and evidence boundaries.

`test_numeric_ordering_result` and `installed_numeric_ordering_result` each
pass 1/1. The independent ordering oracle passes 2,072 cases in both Strict and
Apple profiles. The checks do not claim x86 arithmetic execution, performance
or a complete q semantic-facet/error/resource matrix. The specification
remains Proposed.
