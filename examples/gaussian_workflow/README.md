# Strict Gaussian Result workflow

This public workflow example declares and binds a `Result` input with one Float64 tensor of shape `[side,side]`.
It executes `filter.gaussian_baked64_v1_strict_cpu_whole`, `filter.gaussian_baked64_v1_strict_cpu_tiled`, or
`filter.gaussian_baked64_v1_strict_gpu` with sigma 1 and radius 2 on both axes, `y_axis=0`, `x_axis=1`,
`boundary="clamp"`, and `cval=0`. Flattened input element `i` is `((i*1709+719)%131071)/65536-1`.

Whole and GPU request and compute the complete Result tensor. Tiled publishes requested `IndependentChunks` in
order. The example's `result_publication` callback acquires each newly covered Result tensor region and copies
its row runs into a client-owned output buffer. Each mode compares those bytes with a separately executed
one-worker Whole Result.

```sh
cmake --build build/kernel-dev --target photospider_gaussian_workflow -j 8
build/kernel-dev/examples/gaussian_workflow/photospider_gaussian_workflow 32 4 5 whole 8
build/kernel-dev/examples/gaussian_workflow/photospider_gaussian_workflow 32 4 5 tiled 8
build/kernel-dev/examples/gaussian_workflow/photospider_gaussian_workflow 32 4 5 gpu 8
```

Arguments are side length (1..4096), workers (1..64), measured repetitions (1..10000), mode (`whole`, `tiled`, or `gpu`), and tile width (power of two, 1..4096). Tiled mode uses the same value for tile height and width. Defaults are `32 4 5 whole 8`. The example size limit bounds this sample program, not the operation's legal tensor domain. Large choices may exceed available resources.

The standalone project requires Photospider `0.32.0` and builds against the installed kernel package:

```sh
cmake -S examples/gaussian_workflow -B build/gaussian-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/gaussian-consumer -j 8
build/gaussian-consumer/photospider_gaussian_workflow 9 4 1 gpu 4
```

The standalone configure, build, and 9-by-9 Metal GPU run pass; the output matches the Whole reference bitwise.
Earlier FreeBSD Intel UHD 770 Vulkan measurements exercised the former Value interface. The current Vulkan
Result path has not been revalidated.

JSON reports median and nearest-rank p95 latency, peak modeled host bytes, issued work, native dispatch and
submission counts, native compute time, native constant bytes, `continuation_polls`, and `peak_active_tasks`.
`continuation_polls` sums per-operation `invocation_count` diagnostics; it counts continuation polls, not
computation callbacks. `peak_active_tasks` is a host diagnostic, not a count of simultaneous CPU workers or tile
stages.

One warmup precedes measured repetitions. Timing includes execution and copying output to the client buffer;
graph and input construction, compilation, statistics queries, and output comparison are outside the timer.
Managed capacity, RSS, and client-owned storage are separate quantities.

GPU mode requires an available native service and never falls back to CPU. If the service is unavailable, the
program returns `77`; CTest treats it as a skip for `installed_gaussian_example_gpu`.

Five focused Gaussian tests pass: coefficient generation, exact arithmetic, Whole workflow, tiled execution, and
native GPU. Eight installed-consumer tests pass, covering Whole and tiled consumers, the example's
Whole/tiled/GPU modes, neighborhood relations, C11, and Result native GPU coverage. The standalone
configure/build/run above is an additional independent check.

Independent MPFR/Fraction validation passes 94 workflows and 707 output words each for CPU Whole, CPU tiled, and
native Metal GPU. Coefficient validation passes 312 cases. Typed Result checks cover retained image schemas and
generic ColorArray v1 facets, ICC resources, batch axes, downstream image splitting, tuple closure, ROI support,
Empty output without payload allocation, and Result access after context retirement. These finite fixtures do
not establish the whole mathematical domain or validate the migrated Vulkan path.

[Runtime contracts](../../docs/built-in_ops/05-filter/gaussian-implementation.md) and [independent numerical
checks](../../oracle/ops/filter/README.md) describe parameter, rounding, resource, and correctness behavior.
