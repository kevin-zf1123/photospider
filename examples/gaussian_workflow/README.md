# Strict Gaussian workflows

This standalone public API example binds a Float64 `[side,side]` tensor, compiles
`filter.gaussian_baked64_v1_strict_cpu_whole`,
`filter.gaussian_baked64_v1_strict_cpu_tiled` or
`filter.gaussian_baked64_v1_strict_gpu`. CPU modes use the selected host-worker
quota; GPU mode uses the native backend selected by the kernel build (MSL/Metal or
SPIR-V/Vulkan) and has no fallback. The FreeBSD Intel UHD 770 Vulkan workflow is
validated; NVIDIA and Linux Gaussian Vulkan runs remain untested. The explicit
parameters are sigma 1 and radius 2 on both axes,
`y_axis=0`, `x_axis=1`, `boundary="clamp"`, `cval=0`. Input element i is
`((i*1709+719)%131071)/65536-1`.

```sh
cmake --build build/kernel-dev --target photospider_gaussian_workflow -j 8
build/kernel-dev/examples/gaussian_workflow/photospider_gaussian_workflow 32 4 5
build/kernel-dev/examples/gaussian_workflow/photospider_gaussian_workflow 32 4 5 tiled 8
build/kernel-dev/examples/gaussian_workflow/photospider_gaussian_workflow 32 4 5 gpu
```

The kernel build selects the native GPU backend. Configure with
`-DPHOTOSPIDER_ENABLE_VULKAN=ON` to make `gpu` mode exercise SPIR-V/Vulkan; otherwise
the available Metal build uses MSL.

Arguments are side length (1..4096), workers (1..64), and measured repetitions
(1..10000), mode (`whole`, `tiled` or `gpu`), and tile width/height (power of two,
1..4096). Defaults are `32 4 5 whole 8`. The example size cap bounds the sample program,
not the operator's legal domain. Large choices can exceed available resources.

An installed kernel package supports an independent build:

```sh
cmake -S examples/gaussian_workflow -B build/gaussian-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/gpu-install"
cmake --build build/gaussian-consumer -j 8
build/gaussian-consumer/photospider_gaussian_workflow 16 4 3
```

Tiled mode uses `execute_stream`, validates each bounded tile, and assembles its
rows into the client buffer. Each callback is single-threaded; the shared kernel
pool schedules independent tiles.

Every result is byte-compared with a separately executed one-worker reference.
There is one warmup; JSON reports median and nearest-rank p95 latency, modeled
peak host capacity, issued work and callback diagnostics. Native dispatches,
submissions, device microseconds and constant bytes describe the final repetition. Timing includes
execution and copying output to the client byte array. Graph/input/context setup,
compilation, statistics queries and output comparison are outside the timer.
`peak_active_tasks` reports scheduled operation stages, not the Whole range's
helper-thread count. Allocation capacity, RSS and client-owned storage are
separate quantities.

[Runtime contracts](../../docs/built-in_ops/05-filter/gaussian-implementation.md)
and [independent numerical checks](../../oracle/ops/filter/README.md) describe
legal parameters, one-round arithmetic, budgets and correctness evidence.
