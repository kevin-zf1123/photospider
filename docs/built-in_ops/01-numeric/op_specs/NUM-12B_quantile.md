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
validated as static metadata. Active q must be finite in `[0,1]` as a caller numerical precondition. No runtime guard checks q before index/shift arithmetic; no out-of-domain diagnostic is guaranteed.

The `values` output is a `Result` with schema `photospider.tensor` and tensor key
`samples`. It has the selected output dtype and keeps the source rank and shape,
with the selected axis extent set to 1. Batch axes become ordinary output axes;
output batch topology are empty. The full source rank is 1..8 and its
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
rounded once to the output dtype. All profiles use exact final RN rounding for this dyadic denominator. No approximate quantile sketch,
rank-error tolerance or NUM-14 certificate is used.

## NaNs, infinities and zero signs

The complete active source line is ordered and checked for NaNs before selecting
the rank, including for q=0 or q=1. If a line contains a NaN, return the first
NaN in original axis order, quieted and converted to `dtype` under the
[reduction NaN payload rule](NUM-11_reduction_contract.md). Stable sorting keeps
NaNs after non-NaNs and preserves their original order. The q precondition applies even when the source contains NaNs.

When `w == 0`, convert the selected sample directly. This preserves its signed
zero and correctly converts a selected infinity. For `0 < w < 1`, two
same-sign infinities or one infinity paired with a finite value yield that
signed infinity. Opposite-sign infinities yield the fixed positive quiet NaN.
Two negative-zero interpolation endpoints produce negative zero; all other
exact zero mixtures produce positive zero. Nonzero exact results that underflow
retain their mathematical sign. Infinities outside the selected pair do not by
themselves produce NaN.

Finite source values can round to signed infinity when narrowing to Float32. All source IEEE bit patterns are supported; the finite-domain restriction in the NUM baseline does not apply to source samples of this operation.

## Fixed execution and q projection

Each selected output tile becomes a fixed CPU task. Its source rectangle keeps the output's non-axis intervals and expands the source axis to its complete length L. Source tile closure uses the source's own grid and includes every sample on those lines, even for q=0 or q=1. Tile rounding can include additional samples, which are not processed. Unselected lines impose no payload dependency except through tile rounding.

For L>1, each task also reads q's scalar tile. For L=1, metadata still checks q's complete shape/dtype, but lowering records only a schema reference. q payload and its producer do not participate. Empty selection emits no task or payload reads.

## Scratch, validation and failure

Each invocation reserves `sizeof(StableOrderWorkspace) + sizeof(ExactQuantile) + 16*L` scratch bytes. It reuses the same key/permutation buffers across lines of the tile. Exact arithmetic preserves integer endpoint precision and weights until the final conversion. Output tiles have independent dense allocations. Runtime creates no payload or permutation allocation; Root admits the maximum scratch per CPU lane and selected output storage, plus executor metadata.

Prepare verifies complete input and output schemas, canonical Host writes, exact source/q read membership, empty task parameters and scratch. A malformed static parameter returns InvalidArgument; incompatible schema or task signature returns TypeMismatch. Source, resource, backend and cancellation failures retain their operational statuses. Cancellation is checked during classification, comparisons and writes. Operational failure prevents successful publication of the IR call's outputs; accepted work drains before storage release. Returned immutable owners can outlive ExecutionContext, and concurrent Runs share only immutable prepared metadata. The invocation restores floating modes and exception flags.

## Acceptance

Compare with the retained [NUM12 Fraction oracle](../../../../oracle/ops/numeric/num12/reference.py). Include integer endpoints near Int64 limits, q=0/1, -0, subnormal q and adjacent binary values around integral ranks. Test both output dtypes, Float64-to-Float32 overflow, first-source NaN priority, same/opposite infinities, signed-zero ties and underflow. Check sparse line closure, non-last axes, rank eight, batch/atomic geometry, negative strides, length-one q producer omission, concurrent Runs, artifact import, rebinding, cancellation, resource rejection and malformed prepare signatures. See [fixed ordering execution](../ordering-execution.md).
