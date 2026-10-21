# Fixed reduction execution

The 21 `numeric.reduce_*` keys lower to fixed ABI 8 Host CPU tasks. Their [shared contract](op_specs/NUM-11_reduction_contract.md) defines dtype, axes, keepdims, NaN payload conversion and exact arithmetic. Profile selection is explicit: strict, Apple Silicon and x86-64. Unavailable CPU profiles fail before execution; these keys provide no native GPU placement.

## Group coverage and preparation

Each input contains one tensor under any member key. Its complete `sample_shape()` includes batch axes, with rank 1..8 and at most 2^40 elements. Output `values` uses `photospider.tensor` / `samples`, keeps rank, sets reduced extents to one and uses ordinary axes.

Lowering creates one task for each selected output tile. For a `[5,7]` input reduced along axis 1 with extent 2, an output tile at rows 2..3 reads the input tiles covering rows 2..3 and columns 0..6. It computes those two groups and writes an independent `[2,1]` allocation. Input spatial, batch and atomic topology determines the actual source tile set. Empty selection creates no task.

Prepare checks the kernel symbol/contract, parameters, scratch size, complete input slots and output view. It binds an immutable context containing the reduction mask, group count, ddof, dtypes and read-slot tables. Invocation traverses each group in logical row-major order, independently of tile order or signed physical strides. Separate invocations have private mutable state, including concurrent tasks and Runs.

## Arithmetic, count and ownership

`ReductionState` holds an exact aggregate or exact moments workspace in declared per-lane scratch. The 4352-bit integer machinery preserves exact sums and squared sums; final ratio or squared-midpoint conversion rounds once. Standard deviation rounds the exact square root without first rounding variance. Source NaN priority, payload conversion, signed zero and infinity rules follow the shared specification. Integer final representability is an unchecked caller precondition.

`reduce_count` records `read_input_schema`, with no input payload tile or producer dependency. It fills each selected dense Int64 tile from its static count and uses zero arithmetic scratch. The source sample call remains elided if no other consumer requires it. The output allocation cost is eight bytes per selected sample, independent of input payload size.

The executor reserves output tiles and per-lane scratch before issue. Numerical kernels borrow fixed input views and allocate no payload. They query cancellation at group boundaries and exact-arithmetic checkpoints. Failed Runs discard final RET; accepted tasks retain their input/output/scratch owners until completion. Successful Results retain their independent allocations beyond the context lifetime. Per-value NumericDiagnostics and a per-sample execution-work counter are unavailable in this fixed invocation path. Block/graph resource limits and structural work bounds remain enforced by the kernel.

## Validation scope

Use temporary public `Compiler::plan` -> `prepare` -> `submit` -> `join` programs with independent Fraction totals/moments and integer squared-midpoint square-root rounding. Check all supported dtype conversions, all seven operations, axes order, singleton groups, nonzero selected tiles, empty coverage, rank 1/3/8, batch/atomic axes, signed strides and repeated/concurrent invocations. Compare raw bits for NaN payload conversion, signed zero, subnormals, exact cancellation and finite overflow.

Count checks should use an unexecuted source with a 2^40-element schema and a small selected output set. Invocation-contract checks should reject omitted reads, incorrect scratch and unknown kernel contracts before submission, including after artifact round trips. Static checks cover malformed axes, incompatible dtype and invalid ddof. CPU results do not establish x86 hardware execution, GPU support, maximum-shape throughput or an RSS bound. Preserve the independent numerical oracle; remove temporary checkers after use.
