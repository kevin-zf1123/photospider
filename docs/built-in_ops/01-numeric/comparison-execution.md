# NUM-07 fixed comparison and selection execution

All 24 NUM-07 profile definitions lower to fixed CPU tile tasks: six ordinary comparisons, `is_close` and `select`, each with strict, Apple Silicon and x86-64 profiles. Each input Result contains one tensor member. Inputs have equal complete `sample_shape()` including batch axes, rank 1..8, positive extents and at most 2^40 samples. Profile availability is checked before execution.

## Dtypes and numerical semantics

The six ordinary comparisons accept matching UInt8, Int64, Float32 or Float64 inputs and produce UInt8 0/1. Integer order uses exact integer keys, including values beyond 2^53. Floating values are classified by their bits: signed zeros compare equal, infinities follow numerical order, and any NaN makes only `not_equal` true. Signaling NaNs do not undergo floating evaluation.

`is_close` accepts matching Float32/Float64 inputs. Required static `atol` and `rtol` have Float64 type. Their finite, nonnegative numerical domains are unchecked caller premises, including for empty output selection. Prepare decodes them into immutable binary parts. The fixed workspace compares `abs(a-b) <= atol + rtol * max(abs(a),abs(b))` as exact scaled integers, with no floating overflow, underflow or intermediate rounding. NaNs produce false; same-sign infinities produce true; other infinity pairs produce false.

`select` accepts UInt8 conditions and matching UInt8, Int64, Float32 or Float64 value branches. The caller supplies 0/1 conditions as an unchecked precondition. The kernel copies the selected branch's raw bits, preserving signaling NaNs, signed zero and integer representations. It performs no sample validation.

## Plan and invocation

Each selected output tile generates one task. Lowering declares all intersecting input tiles, independently for each port's batch, spatial and atomic grid. Both select branches remain fixed dependencies. The output port `values` has schema `photospider.tensor` v1, member `samples`, with the complete shape as ordinary axes. Each materialized output tile owns a separate dense allocation.

Prepare verifies the kernel contract, read/write slots, per-port dtype, complete regions, task parameters and scratch. The shared prepared state contains immutable grids, slot maps and scalar facts. The runtime borrows validated affine views and writes directly to the preallocated output. Empty selection emits no tasks; sparse selection computes only the chosen complete tiles.

Comparisons receive preallocated `sizeof(ComparisonMath)` Host scratch per active worker lane and query cancellation between batches of at most four samples. Select requires no task scratch and queries cancellation at most every 1024 samples. Coordinates and cached view pointers are bounded stack data. Independent Runs share prepared facts and retain separate mutable invocation state. Accepted callbacks keep input/output/scratch owners until return; successful Results survive context destruction.

## Validation scope

Temporary public checks should use independent integer/IEEE relations and exact rational `is_close` references, plus raw-bit selection expectations. Cover integer extrema, tolerance equality boundaries, subnormal differences/products, infinities, signaling NaNs, signed strides, unequal port dtypes where allowed, different tile grids, sparse/empty selection, concurrent Runs and malformed task signatures rejected at prepare. Run each accelerated profile only on its supported hardware and report actual execution separately from source availability.
