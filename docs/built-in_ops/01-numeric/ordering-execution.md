# Fixed ordering execution

The six `array.sort_<profile>` and `numeric.quantile_<profile>` keys lower to fixed CPU tasks. Source tensors have UInt8, Int64, Float32 or Float64 elements, complete sample rank 1..8 and at most 2^40 samples. Each input Result has one tensor member with any member key. Static `axis` addresses the complete sample shape, including batch axes. Outputs use ordinary axes and `photospider.tensor` v1/member `samples`.

## Selected lines and output allocations

Sort has independently selectable `values` and Int64 `indices` outputs with the complete source shape. For each selected output tile, lowering removes its axis interval from the grouping key. All selected tiles with the same remaining origins and extents share one task, including tiles from both output ports. The task reads the complete source axis for that non-axis rectangle, classifies and sorts each line once, and writes only its declared output tiles. For example, selected portions of one row from values and indices share the same permutation even when their sorted-axis intervals differ.

Quantile keeps the source shape with axis extent 1 and required static `dtype` of `float32` or `float64`. Each selected output tile reads the corresponding complete source lines. Its q input has complete sample shape `[1]` and Float32/Float64 dtype. Axis length one records only a q schema reference and omits its payload and producer dependency; both input schemas remain fully checked. Longer axes read q tile zero in each task.

Source tile closure follows the source's own batch, spatial and atomic geometry. Tile rounding may include samples outside the exact logical rectangle, while arithmetic visits only its lines. Every selected output tile owns an independent dense allocation. Empty selection emits no tasks or payload reads. Selecting outputs from an existing Plan retains every read, write and scratch requirement of each retained task; compiling a new narrower selection can create smaller groups.

## Stable ordering and quantile arithmetic

`StableOrderWorkspace` builds keys and a permutation in preallocated scratch. Iterative heapsort orders `(numerical key, original axis index)`. Signed integers retain exact order; floating infinities have ordinary numerical order, signed zeros compare equal, and all NaNs compare equal after non-NaNs. Original indices break ties. Sort copies source bits unchanged, including signaling NaNs and payloads.

Quantile decodes q as an exact binary rational and computes `h=(L-1)*q`, its integer position and remainder without preliminary sample conversion. Exact dyadic interpolation receives one final round-to-nearest, ties-to-even conversion. All three profiles use this exact final path. Full selected lines are classified even for endpoint q; the first NaN in original axis order is quieted and converted under the reduction payload rule. Integral positions preserve selected signed zero and infinity. Fractional positions follow the IEEE infinity and zero-sign rules in [NUM-12B](op_specs/NUM-12B_quantile.md).

Active q must be finite in `[0,1]` as a caller numerical precondition. Index and limb-shift arithmetic use q directly without a runtime guard; callers have no guaranteed error behavior outside the numerical domain. All source bit patterns are accepted. Finite Float64 source values can round to signed infinity in a Float32 output. Floating rounding mode and exception flags are restored after invocation.

## Scratch, validation and failure

For axis length L, sort scratch is `sizeof(StableOrderWorkspace) + 16*L` bytes. Quantile adds `sizeof(ExactQuantile)`. The two word buffers hold L keys and L original positions; they are reused for every line in the task. Runtime creates no payload buffer, permutation vector or complete internal output. Ordering takes O(L log L) work per visited line; quantile adds bounded exact arithmetic. Cancellation is checked during classification, comparisons and writes.

Prepare verifies both public sort schemas or the single quantile schema, canonical Host write regions, port/member/dtype, identical non-axis regions within a task, exact complete input tile membership, empty task parameters and exact scratch bounds. It rejects malformed structures before invocation. Each Run borrows immutable prepared metadata and owns its scratch and outputs. Root admission reserves selected output allocations and the maximum task scratch per CPU lane, plus executor metadata. A long axis can therefore exceed capacity even with sparse output selection.

Selected tasks belong to one IR call. An operational failure prevents that call's successful publication; completed private writes are discarded after accepted work drains. Successful Result owners can outlive the execution context. Static malformed parameters fail InvalidArgument; incompatible schemas or task signatures fail TypeMismatch. Source, resource, backend and cancellation failures retain their operational categories.

## Behavior validation

The independent reference is [NUM12's Fraction oracle](../../../oracle/ops/numeric/num12/reference.py). Temporary public execution checks cover raw sort values and indices, exact quantiles, NaN priority and conversions, signed zeros and infinities, Int64 cancellation, subnormal q, and q near zero/one. Exercise non-last axes, rank eight, disjoint output selections, nonzero origins, tail tiles, batch/atomic source geometry, negative and singleton-zero strides, and unaligned storage offsets.

Check the actual source tile closure and shared task count, artifact round trips, concurrent Runs, caller floating environment, q producer omission at length one, source rebinding, resource rejection, cancellation, malformed prepare signatures and output lifetime. Platform and performance claims require their own runtime evidence.
