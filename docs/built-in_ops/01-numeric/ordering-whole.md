# NUM-12 Result Run-shared complete-input computation

The six formal `array.sort_*` and `numeric.quantile_*` keys consume tensor members from `Result` inputs and publish `Result` outputs named `values` and, for sort, `indices`. Outputs use schema `photospider.tensor` with tensor key `samples`. A nonempty active output uses the complete-input tensor continuation: it requests complete active inputs, computes complete lines and publishes the complete output before projecting to a consumer request. Empty requests certify no samples and perform no input payload read or sample arithmetic.

## Tensor ports and demand

Each input `Result` has exactly one tensor member at index zero; its `ResultTensorSpec::key` may be any key. `sample_shape()` includes all declared batch axes. Every source tensor has rank 1..8 and at most 2^40 logical elements. The operation uses that full sample shape, including batch axes, as an ordinary output cell shape; output batch topology are empty.

Sort accepts UInt8, Int64, Float32 or Float64 and requires a static Int64
`axis` in the full sample-shape rank. Output `values` preserves the source dtype
and full shape; `indices` uses Int64 with the same shape and original sorted-axis
coordinates. Both use `photospider.tensor` v1/member `samples`, with batch axes
flattened into ordinary output axes.

The values and indices ports are independently selectable public outputs backed by one prepared joint stage. Within a Run, that stage sorts each line once and produces both complete internal tensors, even when only one output is selected. Output actors publish retained views of requested complete tiles. Both internal output allocations are admitted; optional cross-Run reuse does not provide the unique-producer guarantee.

Quantile takes a source tensor of the same four types and a `q` tensor of Float32 or Float64 with `sample_shape() == [1]`. Its required static parameters are Int64 `axis` and String `dtype` (`float32` or `float64`). The `values` Result keeps the source rank and changes the selected axis extent to 1. For axis length N>=2, ordinary member tile Needs cover the complete source and q tensors, with any explicitly bound validators. The continuation checks q in the operation, after upstream Need completion; a source producer failure can therefore precede the q-domain error.

For quantile axis length 1, metadata specialization still checks q's tensor schema, dtype, axis and output dtype, but selects only source port zero for runtime work. The continuation does not request q payload or record a runtime tile or descriptor dependency on q. A failing q producer is consequently outside that execution. This is a static input projection, not a special q value.

For each nonempty selected output, the complete-input continuation first issues one Need for the complete sample set of each active input through ordinary member tile Needs, then reads through the authorized Result windows and publishes through one direct tensor writer. Sparse consumer output regions do not reduce line sorting, source typed validation or arithmetic. Frozen evidence records the requested tiles of each active input; an active input change can invalidate every dependent output tile. Public actors expose only requested complete tiles. Empty requests issue no payload Need and run no sample callback.

## Sort ordering

Each line is ordered by the tuple `(numerical key, original axis index)`. Integer keys preserve exact signed order. Floating keys place numerical values, including infinities, in ascending order; the two signed zeros compare equal. Every NaN sorts after every non-NaN and NaNs compare equal to one another. The original index breaks all ties, so zeros and NaNs retain their source order. Sort copies the original value bits, including signaling NaNs and payloads, without converting values through floating arithmetic. The `indices` output contains the corresponding original axis coordinate.

The implementation classifies each source sample once into a numerical key
and uses iterative stable heapsort over `(key, original axis index)`.
Comparisons reuse those keys. Each line of length L takes O(L log L) ordering
work and builds one permutation in the joint stage, reused when writing both
internal outputs. Selecting both public outputs does not repeat the sort. Activity
and backing remain scoped to that Run; cross-Run caches are optional.

## Quantile position and result

For a line with N values, sort by the same stable order, then compute the rank from q exactly:

$$
h=(N-1)q,\qquad j=\lfloor h\rfloor,\qquad w=h-j.
$$

When `w == 0`, select sorted value `x[j]`. Otherwise interpolate

$$
y=(1-w)x[j]+wx[j+1].
$$

The q bits are decoded as a binary rational; no floating rank calculation or preliminary Int64-to-float conversion is used. For finite endpoints, the exact selection or interpolation is rounded once to the requested Float32 or Float64 output. Strict uses exact final rounding. Accelerated profiles may use their verified quotient path under the shared [FP32 four-ULP contract](op_specs/NUM_accelerated_contract.md); unresolved cases use strict rounding. There is no approximate sketch, rank tolerance or NUM-14 certificate.

The entire source line is checked for NaNs, including at q=0 and q=1. If a line contains NaNs, return the first one in original logical axis order, quieted and converted to the requested output type under the [reduction NaN payload rule](op_specs/NUM-11_reduction_contract.md). A source NaN does not hide invalid q after successful source preparation. Invalid q is checked before line sorting in the callback.

When the selected position is integral, the chosen sample converts directly, preserving signed zero and converting infinities without interpolation. For a fractional position, two same-sign infinities or one infinity paired with a finite value produce that signed infinity; opposite-sign infinities produce the fixed positive quiet NaN. Two negative-zero endpoints produce negative zero; other exact zero mixtures produce positive zero. A nonzero exact result that underflows retains its sign. Infinities elsewhere in the line do not force a NaN unless the selected interpolation uses opposite infinities.

## State, work and resource ownership

The callback allocates fixed `OrderingState` through the phase allocator's Payload capacity. It contains comparison state and exact quantile arithmetic state. For each active line of length L, key and permutation vectors use 16*L element bytes plus allocator headers, alignment and Entries reservations; they are charged as Root metadata. Key storage is released after permutation construction, and the permutation is released after the line is emitted. Sorting uses one permutation per line rather than 16 times the complete input payload.

Authorized active input windows and the selected full output payload coexist with callback state. The operation does not first collect or pack the complete inputs. Sort values reads source values in permutation order; sort indices writes permutation entries directly. Quantile reads the selected sample or pair for final selection/interpolation after the line order and NaN position are known. Work is charged through the execution Root during reads, comparisons, permutation construction, output writes and exact arithmetic. Cancellation, work or capacity failure aborts the complete-input writer and releases unpublished output, state and metadata. Source or typed-validation failures retain their original first `Status`. No partial Result coverage is published.

## Current behavior checks and limits

The independent numerical oracle is `oracle/ops/numeric/num12/reference.py`.

Temporary public-path checks should cover stable values and original indices for `[3,1,1,2]`, separate
output selection, sparse requests with complete output publication, non-last
axes and disjoint lines, reversed input strides, and worker/caller floating-
environment preservation. Also cover source/q replacement, q projection
and failure precedence, typed input validation, Empty demand, Root work and
metadata limits, cancellation and Result lifetime. Sort and quantile do not
use staged block sharing.

The retained kernel `test_result_c_block` exercises the generic Result C ABI
block-sharing contract with synthetic operations. Sort and quantile require
their own temporary numerical and public-path acceptance.

No x86 arithmetic execution or performance result is claimed. The current
checks do not cover every q structural-contract, error and resource combination.
These are evidence boundaries, not changes to the Proposed specifications.
