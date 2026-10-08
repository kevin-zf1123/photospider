# NUM-12 Result Whole execution

The six formal `array.sort_*` and `numeric.quantile_*` keys consume tensor members from `Result` inputs and publish `Result` outputs named `values` and, for sort, `indices`. Outputs use schema `photospider.tensor` with tensor key `samples`. A nonempty active output uses the Whole tensor continuation: it requests complete active inputs, computes complete lines and publishes the complete output before projecting to a consumer request. Empty requests certify no samples and perform no input payload read or sample arithmetic.

## Tensor ports and demand

Each input `Result` has exactly one tensor member at index zero; its `ResultTensorSpec::key` may be any key. `sample_shape()` includes all declared batch axes. Every source tensor has rank 1..8 and at most 2^40 logical elements. The operation uses that full sample shape, including batch axes, as an ordinary output cell shape; output batch topology and facets are empty.

Sort takes one source tensor of UInt8, Int64, Float32 or Float64 and a required static Int64 `axis`. It offers two independently selected `Result` outputs: `values`, with the source type, and `indices`, with Int64 values naming original coordinates on the sorted axis. Both preserve the full logical shape. Each selected output callback makes one complete output allocation and sorts each line; selecting both outputs may execute two callbacks and build two permutations.

Quantile takes a source tensor of the same four types and a `q` tensor of Float32 or Float64 with `sample_shape() == [1]`. Its required static parameters are Int64 `axis` and String `dtype` (`float32` or `float64`). The `values` Result keeps the source rank and changes the selected axis extent to 1. For axis length N>=2, one Need covers every sample in the source and q tensor with Data, Validation and Descriptor roles (13). The continuation checks q in the kernel, after upstream Need completion; a source producer failure can therefore precede the q-domain error.

For quantile axis length 1, metadata specialization still checks q's tensor schema, dtype, axis and output dtype, but selects only source port zero for runtime work. The continuation does not request q payload or attach a q runtime data, validation or descriptor relation. A failing q producer is consequently outside that execution. This is a static input projection, not a special q value.

For each nonempty selected output, the Whole continuation first issues one Need for the complete sample set of each active input with roles 13, then reads through the authorized Result windows and publishes through one direct tensor writer. Sparse consumer output regions do not reduce line sorting, source typed validation or arithmetic. The output relation records full Data support from each active input; any active input change invalidates all observed coordinates. Empty requests issue no payload Need and run no sample callback.

## Sort ordering

Each line is ordered by the tuple `(numerical key, original axis index)`. Integer keys preserve exact signed order. Floating keys place numerical values, including infinities, in ascending order; the two signed zeros compare equal. Every NaN sorts after every non-NaN and NaNs compare equal to one another. The original index breaks all ties, so zeros and NaNs retain their source order. Sort copies the original value bits, including signaling NaNs and payloads, without converting values through floating arithmetic. The `indices` output contains the corresponding original axis coordinate.

The implementation classifies each source value once into a key and uses iterative stable heapsort over the key/index permutation. Comparisons reuse those keys; each line of length L takes O(L log L) ordering work. One permutation is built for each line during each selected-output callback. No permutation is shared across the independently selected sort outputs.

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

Authorized active input windows and the selected full output payload coexist with callback state. The operation does not first collect or pack the complete inputs. Sort values reads source values in permutation order; sort indices writes permutation entries directly. Quantile reads the selected sample or pair for final selection/interpolation after the line order and NaN position are known. Work is charged through the execution Root during reads, comparisons, permutation construction, output writes and exact arithmetic. Cancellation, work or capacity failure aborts the Whole writer and releases unpublished output, state and metadata. Source or typed-validation failures retain their original first `Status`. No partial Result coverage is published.

## Current behavior checks and limits

The current public workflow is `examples/numeric_workflow/ordering.cpp`; it is
registered as `test_numeric_ordering_result`. The independent numerical oracle
is `oracle/ops/numeric/ordering_oracle.py`. Build and run the strict example,
the oracle for strict and locally available accelerated profiles, and the
focused CTest with:

```sh
cmake --build build/kernel-dev --target photospider_numeric_ordering -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_ordering strict
python3 oracle/ops/numeric/ordering_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_ordering strict
python3 oracle/ops/numeric/ordering_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_ordering apple
ctest --test-dir build/kernel-dev -R '^test_numeric_ordering_result$' --output-on-failure
```

The executable accepts `strict`, `apple` and `x86`; accelerated profiles need a
compatible host and otherwise return `BackendUnavailable`.

The workflow checks stable values and original indices for `[3,1,1,2]`, separate
output selection, sparse requests with complete output publication, non-last
axes and disjoint lines, reversed input strides, and worker/caller floating-
environment preservation. It also covers source/q replacement, q projection
and failure precedence, typed input validation, Empty demand, Root work and
metadata limits, cancellation and Result lifetime. Its `block_contracts()` case
is separate from the formal Whole operations: a pure dependency-v2 two-output
probe tests the `share_blocks_across_outputs` trait, cache-disabled
recomputation, optional cache-work fallback, changed input bits and current
per-output Need evidence. Sort and quantile themselves do not use staged block
sharing.

The installed consumer compiles the same workflow source against
`Photospider::kernel`:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-repeat-install
```

 The separate
`test_result_c_block` and installed `installed_result_c_block` also pass 1/1;
they exercise the generic Result-v2 block-sharing contract, not the Whole sort
or quantile callbacks.

No x86 arithmetic execution or performance result is claimed. The current
checks do not cover every q semantic-facet, error and resource combination.
These are evidence boundaries, not changes to the Proposed specifications.
