# Numeric operations

The default registry in `make_default_operation_registry()` provides these CPU
operations through the public WorkflowDocument, Compiler and ExecutionContext
interfaces. The accepted boundary is [ADR 0020](../adr/0020-composable-operation-foundations.md).
The [Chinese mirror](zh/Numeric-Operations.zh.md) describes the same implementation.

Clamp and arithmetic use Whole input/output demands. Reductions
read complete logical input support through sequential bounded stages. All return
packed generic Values with empty facets. Rank-1..8 nonzero shapes remain required. Clamp
and arithmetic preserve shape; reductions return Float64 `{1}`. Binary inputs
must have identical dtype and shape. There is no implicit broadcasting, casting
or semantic preservation. These descriptions apply to generic arrays and
admitted non-image inputs. Legacy coverage/image Values are rejected by the
planar gate; multiplying a legacy coverage mask is not a supported adaptation.

| Key | Inputs | Static parameters |
| --- | --- | --- |
| `numeric.add`, `numeric.subtract`, `numeric.multiply`, `numeric.divide` | Two Float32 or two Float64 arrays | None |
| `numeric.clamp` | One Float32/Float64 array | Finite inclusive Float64 `min`, `max`, with `min <= max` |
| `numeric.mean`, `numeric.variance` | One Float32/Float64 array | Optional Int64 `block_size` in [1,65536], default 64 |
| `numeric.ordered_scan` | One rank-1 Float64 array; same-shape generic output | Optional Int64 `block_size` in [1,65536], default 64 |

Package 0.20.0 removes `numeric.cast` and `numeric.encode_range`. Numeric
format conversion belongs to the proposed FMT-06 family; see the
[retirement record](../built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md).

Arithmetic computes in the input dtype, rejects non-finite inputs/results and
rejects division by either signed zero. Clamp checks the selected result before
Float32 narrowing; large unused Float64 endpoints are legal. Mean accumulates in
Float64 in fixed logical row-major order; population variance uses two passes
(mean, then squared deviations, `ddof=0`). Non-finite accumulated results fail.
There is no implicit parallel or reassociated reduction.

Mean/variance use the staged dependency protocol with one scalar observation.
`block_size` bounds requested samples per phase. Exact row-major intervals
decompose across rank-1..8 axes
without reading a bounding-box gap. Each block continues the incoming Float64
accumulator directly, preserving the order and global sample index of nonfinite
input, sum-overflow and variance-overflow checks. Variance finishes the first
pass before retaining its exact mean for every second-pass block. Block size is
an input-read granularity, not a batch of output observations.

Admitted non-image semantic inputs use the supplied-fragment validator before
arithmetic. Legacy image rejection does not establish planar reduction support.
Opaque vendor facets do not add a validation scan. All state and
live fragments use the current ExecutionContext worker/admission/allocator.
Empty exact scalar queries read nothing; resource/discovery/cancellation bounds
remain explicit. Source data can exceed the live payload budget when its blocks
fit. Completed exact-demand cache hits retain the complete global source support;
changing any observed input invalidates the scalar result. Completed internal transitions can also reuse the block cache described below.

`test_ordered_reduction` checks bitwise results over five block sizes and ranks
1, 4 and 8, Float32/Float64, cache cold/warm, original error sample indices,
second-pass cancellation/recovery, legacy image rejection, and a 32 KiB source
under a 1 KiB controlled live budget. Its independent arithmetic oracle uses an
explicit binary64 left fold. The [G4 public workflow](../../examples/g4_workflow/README.md)
checks mean 1.5 and variance 1.25 over repeated `[0,1,2,3]` with bounded source reads.

`numeric.ordered_scan` computes inclusive prefixes with a positive-zero Float64
initial carry, strict left-to-right addition, nearest-even rounding and gradual
underflow. It restores the caller's environment. Output j observes input `[0,j]`;
no read or arithmetic extends past j. The first nonfinite input or accumulator
fails with `nonfinite scan input i` or `scan overflow i`. Therefore `[1,inf]`
queried at `{0}` succeeds with 1, while `{1}` or `{0,1}` fails at input 1.
RequestFailureOnly still invokes each output independently.

Successful prefix carries can be reused through completed-only checkpoints in
the same active input bundle. Each borrowed checkpoint imports its full direct
input witness and upstream structural records. There are no cached failures or
worker waits. Checkpoints use existing host allocator leases and bounded optional
metadata retention; eviction can cause recomputation. A dense 256-output source
test reads exactly 256 inputs once. Private execution-hook tests hold a real
published prefix to check both waiter start orders, owner cancellation, warm
result caching and exact imported source support. Direct/manual protocol tests
check allocator ownership, scope/sequence rejection and witness limits.
Scan and mean/variance now retain completed internal transitions across bundles
through the existing result LRU. Keys include exact supplied sets/input bits,
actual incoming state bits, phase, range and fixed nearest-even/gradual numeric
mode. Variance includes its fixed mean in every second-pass incoming state.
The host hashes supplied fragments after their normal validation; a hit copies
state into the current stage allocator and retains current dependency evidence.
Only successful transforms are stored, with no additional input reads or output
batching. Changed incoming state forces the current block to recompute. Later
blocks may hit after their incoming state reconverges and their inputs match.
`block_cache_hits/misses` count these internal lookups separately from completed
output `cache_hits`; optional cache-work exhaustion skips lookup/retention.
The public block workflow and tests verify the `[1,2^54]` reconvergence boundary,
frozen/current output differences, and second-pass invalidation when mean changes.
Completed output caching still verifies its full transitive prefix support.

The implementation reads logical coordinates using storage origin, byte offset
and signed strides, including unaligned and zero-stride views. It allocates
outputs through the invocation allocator, checks representable dense output
before IR publication, polls cancellation during traversal and before publishing,
and releases unpublished buffers on errors. Invalid parameters return
`InvalidArgument`; unsupported/mismatched dtype or shape returns `TypeMismatch`
before callback entry; invalid numeric results return `OperationFailed` with a
sample index where applicable. Resource exhaustion and cancellation retain their
own codes. No partial successful Value is published.

## Public workflow and validation

[test_numeric_operations.cpp](../../tests/integration/test_numeric_operations.cpp)
constructs public producer-to-operation workflows and checks arithmetic, clamp,
mean/variance, strided and unaligned inputs, type/shape rejection, resource
failure and cancellation. Expected values include `[3,2,1]-[4,4,4]=[-1,-2,-3]`,
mean `[1,2,3]=2` and population variance `2/3`.

```sh
cmake --build build --target test_numeric_operations -j 8
ctest --test-dir build -R '^test_numeric_operations$' --output-on-failure
```

The [foundations example](../../examples/foundations_workflow/README.md) runs
maintained generic numeric, expression/LUT and field-filter workflows. The
[retirement regression](../../tests/integration/test_format_color_retirement.cpp)
checks that removed format keys cannot compile and demonstrates a minimal bound
`numeric.add_strict` graph with result 0.5. New format conversion specifications
are not runtime interfaces.
