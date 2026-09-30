# Numeric operations

The default registry contains legacy unsuffixed arithmetic keys and newer profile-specific numeric families. The families have different parameter, region, and reduction contracts; choose a key by its exact name rather than assuming a suffix is cosmetic. Operation plugins use C ABI 11.

## Unsuffixed Value operations

| Key | Inputs and output | Parameters and region |
| --- | --- | --- |
| `numeric.add`, `numeric.subtract`, `numeric.multiply`, `numeric.divide` | Two same-shape, same-dtype Float32/Float64 generic Values; output preserves shape and dtype | No parameters; Whole input/output |
| `numeric.clamp` | One Float32/Float64 generic Value; output preserves shape and dtype | Required finite Float64 `min`, `max`, with `min <= max`; Whole |
| `numeric.mean`, `numeric.variance` | One rank-1..8 Float32/Float64 Value; Float64 scalar `[1]` | Optional Int64 `block_size` in `[1,65536]`, default 64; staged dependency reads |
| `numeric.ordered_scan` | One rank-1 Float64 Value; same-shape Float64 output | Optional Int64 `block_size` in `[1,65536]`, default 64; prefix dependency reads |

The four arithmetic keys calculate in the input dtype and reject nonfinite inputs or results. Division rejects either signed zero. Clamp checks the selected result before narrowing to Float32; an unused endpoint may exceed the Float32 range. The callbacks use the invocation allocator and do not broadcast, cast, or preserve semantic facets.

`numeric.mean` accumulates in Float64 in logical row-major order. `numeric.variance` uses two passes: it computes the mean first, then accumulates squared deviations with `ddof=0`. Both expose one scalar output and divide the source into exact logical row-major intervals. A requested block can span tensor axes without reading bounding-box gaps. `block_size` bounds input reads; it does not batch output observations. Nonfinite values and unrepresentable accumulated results fail. The execution context owns continuation state and live fragments; allocator and work limits apply while blocks are processed.

`numeric.ordered_scan` computes inclusive prefixes from a positive-zero Float64 carry with strict left-to-right addition, nearest-even rounding and gradual underflow. Output `j` reads only input `[0,j]`. The first nonfinite input or accumulated overflow fails with its global input index. Successful carries may be reused through completed checkpoints; failures are not cached and checkpoint eviction can cause recomputation.

## Profile-specific pointwise operations

The numeric math families use three explicit suffixes:

| Profile suffix | Admission |
| --- | --- |
| `_strict` | Portable strict implementation |
| `_accelerated_apple_silicon` | Apple Silicon implementation when its runtime profile is available |
| `_accelerated_x86_64` | x86-64 accelerated implementation when its runtime profile is available |

`numeric_binary.cpp` registers each suffix for `add`, `subtract`, `multiply`, `divide`, `minimum`, `maximum`, `pow`, `atan2` and `atan2pi`. The exact elementary kernels accept same-dtype UInt8, Int64, Float32 and Float64 inputs for supported operations; divide is Float32/Float64 only. The certified transcendental functions use Float32/Float64 and their own domain and representability checks. These keys are Whole operations and keep dtype and shape. NaNs propagate according to each operation's exact kernel; domain and range failures follow each family's typed contract. `numeric.clamp_<profile>` and `numeric.remap_range_<profile>` report invalid bounds as `InvalidArgument/InvalidDomain`. Use the strict key when the selected accelerated profile is unavailable or its backend policy is not appropriate.

The suffixed reduction family registers `numeric.reduce_sum`, `numeric.reduce_minimum`, `numeric.reduce_maximum`, `numeric.reduce_mean`, `numeric.reduce_count`, `numeric.reduce_variance` and `numeric.reduce_std`, each with the same three suffixes. These are Whole operations. They require a comma-separated String `axes` list of unique canonical nonnegative axis indices; sum, mean, variance and standard deviation also require String `dtype` (`uint8`, `int64`, `float32` or `float64`), subject to source and operation-domain compatibility. Variance and standard deviation require Int64 `ddof` in `0 <= ddof < N`, where `N` is the product of the reduced extents. Reduced axes become length one; unreduced extents remain unchanged. The implementation rejects invalid axes, unsupported dtype combinations, oversized inputs and invalid degrees of freedom. These reductions have profile-specific exact arithmetic and output conversion. They are separate registered keys from the unsuffixed staged `numeric.mean` and `numeric.variance` above.

`numeric.clamp_<profile>` and `numeric.remap_range_<profile>` use the same suffixes. Clamp takes three same-shape, same-dtype inputs in value, lower-bound, upper-bound order. Remap takes five in value, source-low, source-high, destination-low, destination-high order. Both preserve input shape and dtype, require rank 1..8, use Whole regions and reject invalid bounds during execution. This clamp family is distinct from the unsuffixed two-static-parameter `numeric.clamp`.

The staged mean and variance callbacks keep their continuation state and each live source fragment under the execution context's allocator and resource limits. Completed scalar results retain the full observed input support in their cache evidence. Successful internal blocks can also be reused when supplied input bits, phase, range and incoming accumulator state match; variance includes the first-pass mean in its second-pass state. Failed blocks are not cached. Cache-work exhaustion skips optional lookup or retention, and an evicted block can be recomputed.

The profile-specific pointwise and reduction implementations account work and cancellation through the execution resource budget. A profile that cannot be selected on the host fails during preparation; callers should use the strict suffix when portability is required. Profile names do not promise GPU execution.

## Inputs, ownership and failures

Generic numeric Values have rank 1..8 with nonzero extents and empty facets unless a family declares semantic input validation. Operations read logical coordinates, including legal storage offsets and signed strides, and allocate outputs through the invocation allocator. Output Values are not published until the callback completes and passes its final cancellation check.

Invalid static parameters return `InvalidArgument`. Unsupported or incompatible dtype, shape, or facet metadata returns `TypeMismatch`. The unsuffixed finite-domain arithmetic, staged reductions and scan report nonfinite or unrepresentable results as `OperationFailed`. Profile-specific exact kernels preserve or classify IEEE values according to the operation; typed domain failures can return `InvalidArgument/InvalidDomain`. Resource exhaustion and cancellation retain their own status codes. No operation returns a partial successful Value.

## Public workflows and checks

[`test_numeric_operations.cpp`](../../tests/integration/test_numeric_operations.cpp) exercises the unsuffixed arithmetic, clamp, mean and variance paths, including strided and unaligned input, shape/type rejection, resource failure and cancellation. [`test_ordered_reduction.cpp`](../../tests/integration/test_ordered_reduction.cpp) checks reduction order, block boundaries, cache reuse and cancellation. The public [G4 workflow](../../examples/g4_workflow/README.md) checks mean `1.5` and population variance `1.25` for repeated `[0,1,2,3]`.

```sh
cmake --build build --target test_numeric_operations test_ordered_reduction -j 8
ctest --test-dir build -R '^(test_numeric_operations|test_ordered_reduction)$' --output-on-failure
```

Generic arithmetic keys do not declare planar image capability. Their registration does not imply that a structural planar image can be passed as a generic numeric array.
