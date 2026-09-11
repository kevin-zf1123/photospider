# Numeric operations

The default registry in `make_default_operation_registry()` provides these CPU
operations through the public WorkflowDocument, Compiler and ExecutionContext
interfaces. The accepted boundary is [ADR 0020](../adr/0020-composable-operation-foundations.md).
The [Chinese mirror](zh/Numeric-Operations.zh.md) describes the same implementation.

Cast, range, clamp and arithmetic use Whole input/output demands. Reductions
read complete logical input support through sequential bounded stages. All return
packed generic Values with empty facets. Rank-1..8 nonzero shapes remain required. Cast, range, clamp
and arithmetic preserve shape; reductions return Float64 `{1}`. Binary inputs
must have identical dtype and shape. There is no implicit broadcasting, casting
or semantic preservation: multiplying a coverage mask by two produces a generic
array, which can then be explicitly interpreted by another operation.

| Key | Inputs | Static parameters |
| --- | --- | --- |
| `numeric.cast` | One UInt8/Int64/Float32/Float64 array | String `dtype`: `uint8`, `int64`, `float32`, `float64`; String `rounding`: `ties_even`; String `overflow`: `reject` or `clip` |
| `numeric.encode_range` | One array of any of the four dtypes | Cast parameters plus finite Float64 `src_min`, `src_max`, `dst_min`, `dst_max`; both intervals strictly increasing |
| `numeric.add`, `numeric.subtract`, `numeric.multiply`, `numeric.divide` | Two Float32 or two Float64 arrays | None |
| `numeric.clamp` | One Float32/Float64 array | Finite inclusive Float64 `min`, `max`, with `min <= max` |
| `numeric.mean`, `numeric.variance` | One Float32/Float64 array | Optional Int64 `block_size` in [1,65536], default 64 |
| `numeric.ordered_scan` | One rank-1 Float64 array; same-shape generic output | Optional Int64 `block_size` in [1,65536], default 64 |

Constructors explicitly write `rounding="ties_even"` and `overflow="reject"`
for the default behavior. The registry never supplies missing parameters.
Dither is fixed off; there is no dither parameter. Range encoding evaluates the
affine mapping of the declared intervals; values outside the source interval
are extrapolated. Clip refers to the target dtype limits, not the destination
interval. It is only enabled explicitly.

Cast never rescales. Same-dtype cast preserves sample bits, including signed
zero and supported floating NaN payloads. Integer sources convert directly to
the requested dtype without a binary64 intermediate. Floating-to-integer casts
round ties to even, then check bounds before conversion; Int64's positive
binary64 boundary is exclusive `2^63`. Finite narrowing overflow is rejected
or explicitly clipped to the target's finite boundary. Floating cast accepts
supported NaN/infinity; integer cast and range encoding reject non-finite inputs
under either overflow policy.

Range computation uses Float64 affine arithmetic with a nearby endpoint anchor,
error-free high/low differences, compensated products/sums and explicit `fma`
under nearest-even rounding, with scaled widths for intervals exceeding the
Float64 difference range. Unrepresentable zero/infinite coefficients or unresolved
quotient/accumulation error exceeding one output Float64 ULP fail;
identity intervals preserve the source before checked conversion. Finite source
values whose final affine result overflows obey the selected reject/clip policy,
including floating output. Extreme mappings whose coefficients are not
representable are not supported. The caller's floating environment is restored.

Arithmetic computes in the input dtype, rejects non-finite inputs/results and
rejects division by either signed zero. Clamp checks the selected result before
Float32 narrowing; large unused Float64 endpoints are legal. Mean accumulates in
Float64 in fixed logical row-major order; population variance uses two passes
(mean, then squared deviations, `ddof=0`). Non-finite accumulated results fail.
There is no implicit parallel or reassociated reduction.

Mean/variance use the staged dependency protocol with one scalar observation.
`block_size` bounds requested samples per phase; image inputs round it upward to
complete pixels, and exact row-major intervals decompose across rank-1..8 axes
without reading a bounding-box gap. Each block continues the incoming Float64
accumulator directly, preserving the order and global sample index of nonfinite
input, sum-overflow and variance-overflow checks. Variance finishes the first
pass before retaining its exact mean for every second-pass block. Block size is
an input-read granularity, not a batch of output observations.

Typed input semantics receive a complete validation pass before arithmetic,
using the existing supplied-fragment validator and full image C. Opaque vendor
facets do not add a validation scan. All state and
live fragments use the current ExecutionContext worker/admission/allocator.
Empty exact scalar queries read nothing; resource/discovery/cancellation bounds
remain explicit. Source data can exceed the live payload budget when its blocks
fit. Completed exact-demand cache hits retain the complete global source support;
changing any observed input invalidates the scalar result. Completed internal transitions can also reuse the block cache described below.

`test_ordered_reduction` checks bitwise results over five block sizes and ranks
1, 4 and 8, Float32/Float64, cache cold/warm, original error sample indices,
second-pass cancellation/recovery, typed channel closure, and a 32 KiB source
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

The executable source [test_numeric_operations.cpp](../../tests/integration/test_numeric_operations.cpp)
contains a minimal public producer → operation workflow in `run()`. It registers
immutable input producers (including legal strided Values), submits their
`WorkflowNodeOutput` references, calls `Compiler::compile`, and executes the plan.
This can be replaced by ordinary WorkflowDocument input declarations and per-run
bindings. For example, a bound scalar cast uses only these public objects:

```cpp
#include <cstring>
#include <string>

#include "photospider/photospider.hpp"

int main() {
  auto registry = ps::make_default_operation_registry();
  ps::WorkflowDocument doc;
  doc.inputs = {{1, "input", {ps::ElementType::Float64, {1}},
                 ps::Region::whole({1}), {0, {8}}, {}}};
  doc.nodes = {{1, "numeric.cast", {ps::WorkflowInputReference{1}},
                {{"dtype", std::string("float32")},
                 {"rounding", std::string("ties_even")},
                 {"overflow", std::string("reject")}}}};
  doc.outputs = {{"result", 1, "value"}};
  ps::GraphContext graph(doc);
  ps::Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  if (!compiled.ok()) return 1;
  ps::ExecutionContext execution(registry);
  auto result = execution.execute(compiled.value().plan,
      {{{"input", ps::Value::from_float64(-2.5)}}});
  if (!result.ok()) return 2;
  const auto& value = result.value().values.at("result");
  if (value.descriptor().element_type != ps::ElementType::Float32 ||
      value.bytes().size() != sizeof(float) || !value.facets().empty()) return 3;
  float sample;
  std::memcpy(&sample, value.bytes().data(), sizeof(sample));
  return sample == -2.5F ? 0 : 4;
}
```

Run the maintained executable (replace the build path with an existing configured
build; static and shared kernels use the same source):

```sh
cmake --build build/issue257-static --target test_numeric_operations -j 8
ctest --test-dir build/issue257-static -R '^test_numeric_operations$' --output-on-failure
```

Exit zero checks all 256 UInt8 values through each dtype and range round-trips;
positive/negative halfway and Int64 boundary cases; `Int64→Float32` midpoint
bits `0x5e800001`; descending subtraction `[3,2,1]-[4,4,4]=[-1,-2,-3]`;
mean `[1,2,3]=2`, variance `2/3`; typed mask semantic removal; non-finite,
overflow, shape/dtype, allocation and cancellation errors. Extreme symmetric
range compression checks `±2→±1` independently; adjacent domains above `2^53`
check `2` and `1/3`, and zero-origin extreme domains preserve small positive samples. Tests inspect exact bytes or
stated Float64 tolerance, not a second call to the implementation as an oracle.

Change operation keys/parameters in `run()`, or connect one result node to the
next node instead of supplying another source. To reconnect a single generic
Float32 result to `image.exposure_gain`, keep shape `{1}` and the gain range
`[0,16]`; the bounded consumer validates every computed result before use.
The [channel/color operations](Channel-and-Color-Operations.md) provide this
explicit composition. [Expression and LUT operations](Expression-and-LUT-Operations.md)
provide sampled generators with dynamic coefficients. The [standalone foundations example](../../examples/foundations_workflow)
combines these operations using an installed public package.
