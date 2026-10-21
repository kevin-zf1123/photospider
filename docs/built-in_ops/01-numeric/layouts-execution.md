# NUM-09 fixed layout transforms

The nine `array.{reshape,transpose,slice}_{strict,accelerated_apple_silicon,accelerated_x86_64}` keys use ABI 8 CPU tasks. Every selected output tile has one task and an independent dense allocation. The output port is `values`, using `photospider.tensor` with tensor member `samples`. Input schema/member names may vary. The complete input `sample_shape()` includes batch axes; output axes are ordinary axes. All seven numeric dtypes are copied bit-for-bit, including signaling NaN payloads and signed zeros.

The authoring API has no layout policy parameter. `TransformLayout` and the `layout` node parameter are removed. Helpers accept the numerical profile immediately after shape/permutation/counts. Source owners and physical strides do not select a different execution path. The compiled task reads fixed affine source views and writes its own output allocation. Negative, zero and unaligned source strides/offsets are supported after physical validation. Empty output selection emits no task or payload dependency.

## Static source sets

Reshape preserves row-major logical ordinals. For every requested output tile, lowering partitions its coordinates into contiguous ordinal intervals, then partitions those intervals into aligned input rectangles. Only the two partial boundary chunks recurse at each source axis. This computes the exact intersecting source tiles by intervals; fragmented intervals can each contain one sample. Transpose maps one output rectangle into its axis-permuted input rectangle. Both use the source's own TileGrid, including its batch and atomic topology.

Slice has dynamic Int64 `[rank]` starts and steps, with positive static counts. Lowering fixes every possible input tile that valid controls could select. For one axis with source extent I, count N, and output tile interval `[u,u+e)`, the possible source coordinates are:

```text
N = 1:         [0,I)
1 < N <= I:    [u,I-N+u+e) union [N-u-e,I-u)
```

For positive steps, the possible coordinates are contained in those reachable with step +1; +1 realizes every coordinate in the first interval. Negative steps yield the second interval through step -1. Different axes have independent controls, so at most two intervals per axis produce at most 256 source boxes at rank eight. Overlapping or adjacent axis intervals are merged before taking the Cartesian product. Input tiles intersecting these boxes are deduplicated. This is a possible-source set across valid bindings, not the exact source set for one Run.

Counts greater than source extents have no valid nonzero-step slice and fail static validation. Every nonempty slice task also declares the complete starts vector. Steps are declared completely if any count exceeds one. If all counts are one, static specialization excludes the step payload and producer while still checking its declared schema. An otherwise unused singleton-axis step belongs to that declared input tile set when another axis uses steps.

At runtime each task reads its controls and computes selected source coordinates with widened `__int128` arithmetic. Starts in bounds, nonzero steps for non-singleton axes and complete-domain endpoints in bounds are unchecked caller premises. No input or complete-endpoint validation runs. Singleton steps are ignored. Controls select coordinates within the Plan's possible-source set without changing topology, views or allocation ownership.

## Preparation, execution and lifetime

Preparation checks static output metadata, the exact output tile, complete ordered source/control tile slots, dtype, Host domain, empty task parameters and zero scratch. It recomputes the source set independently from the saved task reads. Missing, extra, duplicate or mismatched reads fail before invocation. Source-set reconstruction caps unique source tiles by the declared read count, checks each compressed region before enumeration and uses the default physical-validation work ceiling. Prepared state owns immutable shape/permutation metadata and slot maps; mutable controls, coordinates and a 32-byte copy buffer are bounded stack objects in each invocation.

The CPU kernel maps each global output coordinate to its source coordinate, reads raw bytes and writes dense output order. Copy batches use the selected profile's byte-copy helper; no floating-point arithmetic or NaN quieting occurs. Cancellation is sampled at entry and every 64 copied elements. Operational errors prevent successful call retirement. Source and output owners remain retained through accepted work; successful Results own storage beyond execution context destruction. There are no per-value NumericDiagnostics, runtime payload allocations, sample work counters, content caches or implicit layout fallbacks in these fixed invocations.

## Validation

Temporary public-path checks use independent integer flatten/unflatten, axis permutation and signed slice enumeration. Compare raw words for all seven dtypes, caller floating environments, batch/atomic input grids, rank changes, negative/zero strides, unaligned offsets and sparse selections. Compare saved source tile sets against an independent possible-coordinate union. Roundtrip Plan artifacts, reuse a PreparedPlan in concurrent Runs, check empty input elimination and all-singleton step elision. Reject invalid static counts/permutations and malformed prepared signatures; exercise valid positive/negative steps and boundary endpoints without requiring diagnostics for violated dynamic premises. Remove temporary checkers after validation; retain the separate numerical oracles. Host CPU profile validation does not establish native x86 or GPU execution.
