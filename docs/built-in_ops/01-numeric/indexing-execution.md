# NUM-10 fixed indexing execution

The eighteen `array.{concatenate,gather,scatter_replace,scatter_sum,scatter_minimum,scatter_maximum}_<profile>` keys use ABI 8 CPU tasks. Profiles are `strict`, `accelerated_apple_silicon` and `accelerated_x86_64`; host availability is checked before invocation. Input Results each contain one tensor under any schema/member key. Their complete sample shapes include batch axes, have rank 1..8 and contain at most 2^40 elements. Source/base/updates use UInt8, Int64, Float32 or Float64. Output `values` uses `photospider.tensor`/`samples` with ordinary axes and independent dense output tiles. Empty selection emits no task or payload dependency.

## Concatenation

Concatenate accepts 2..256 ordered inputs with equal dtype, rank and non-axis extents. Static Int64 `axis` selects the axis whose lengths are summed. The `layout` parameter is removed; the public helper accepts the profile immediately after axis.

Each selected output tile becomes one task. Let P[k] be the sum of preceding input axis lengths. Intersect the output tile with each input slab `[P[k],P[k+1])`, subtract P[k] from that axis, and declare all input tiles intersecting the resulting region. Empty intersections create no payload dependency on that port or its producer. Static metadata for every input is still checked. Runtime maps global output coordinates to their ordered input and copies raw bytes, preserving signaling NaNs and signed zeros. Physical source owners do not select an aliasing path.

## Shared indices and output tasks

A nonempty gather or scatter creates one index task that reads the complete Int64 indices vector `[M]`. Every index is in the source/base axis as an unchecked caller premise, including indices associated with unselected output. The producer constructs the table without a validation scan. This task depends on source metadata but reads no source/base/updates payload. It writes one private SSA tile, Int64 `[M,2]` with both axes atomic, occupying `16*M` bytes. Its successful completion makes that tile available to all output tasks through explicit reads.

Gather stores `{original_position, source_index}` in original index order. It uses no index-task scratch. Each output task declares the source region matching its non-axis output coordinates and covering the complete source axis. It reads the normalized table at its output axis coordinate, then copies the selected source word. Repeated indices preserve output order.

Scatter initially stores `{target_index, original_position}`. Eight stable least-significant-byte radix passes group targets while preserving ascending original position for duplicates. The private output is one radix buffer and a `16*M`-byte scratch allocation is the other; after eight passes the result is in the private output. Each output task declares base tiles intersecting its output region and update tiles covering that region's non-axis coordinates and all M update positions. It locates matching rows by bounded binary searches.

No-hit outputs copy base bits directly. Replace copies the update with the greatest matching original position. Sum/minimum/maximum initialize private exact state per invocation and, for each hit coordinate, add base first and then matching updates in original position order. Source NaN priority and quieting, infinities, signed zeros and exact final conversion follow the scatter contract. Unhit signaling NaNs and replacement values are copied without arithmetic. Integer final representability is an unchecked caller precondition for every computed output tile; unselected output coordinates are not computed.

## Preparation and runtime boundaries

Prepare validates public metadata, exact source/control tile slots, output regions, dtype, Host placement, empty task parameters and precise scratch size. The private view must be exactly dense Int64 `[M,2]`, zero-origin/offset, strides `[16,8]`, and `16*M` bytes, with private port identity. Read-set reconstruction checks compressed set sizes against available read slots before enumeration and has a finite work limit.

The private table's payload structure, normalized positions and association with its source controls are unchecked producer/consumer premises. Gather and scatter consume prepared positions directly without bounds checks, provenance comparisons or a publication scan. Static private-view signatures remain checked during preparation.

Prepared metadata and slot maps are immutable. Index buffers, output tiles and aggregate scratch belong to the current block activation or invocation. The executor reserves each CPU lane's maximum block scratch, so admission includes `16*M` per lane when the scatter sorting task determines that maximum, in addition to private and public tile allocations. Runtime kernels create no payload allocations. Index construction/sorting checks cancellation every 256 items, output copying every 64 samples, and exact arithmetic through its cancellation callbacks. No per-sample work counters, NumericDiagnostics, content cache or continuation state is used. Successful output owners outlive the execution context; accepted tasks retain their inputs through drain.

## Validation

Temporary checks use independent index grouping, exact rational sums with destination rounding, and raw-bit oracles. Cover all six operations, four source dtypes, duplicate targets, nonleading axes, batch/atomic grids, sparse/empty selections, negative/zero strides, unaligned offsets, caller floating environments, artifact roundtrips and concurrent Runs. Compare each Plan's source tiles against the required coordinate union and verify exactly one shared indices task. Exercise 65/256-port concatenation, valid boundary indices outside selected output, dynamic rebinding, unselected producer elision, precise rejection of malformed static preparation signatures and valid normalized-position reuse. Delete temporary checkers after validation; retain the separate numerical oracles. CPU profile checks do not establish execution on unavailable native x86 or GPU devices.
