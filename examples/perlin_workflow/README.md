# Strict Perlin CPU and GPU workflows

This example constructs `coordinates[N,3]`, compiles a public `WorkflowDocument`,
binds a Float64 tensor and runs the selected registered strict implementation.
Whole uses the host range grant. Tiled uses single-thread callbacks scheduled by
`execute_stream`; each callback computes only its requested samples. Stream
publication is ordered and the example checks complete, nonoverlapping coverage.
GPU mode runs the native integer shader selected by the kernel build: Metal uses MSL
and Vulkan uses SPIR-V. Both modes require real native dispatches and reject CPU
fallback. The Vulkan Perlin workflow has been verified on FreeBSD with Intel UHD 770;
the NVIDIA Perlin path and Linux hardware remain untested.

```sh
cmake --build build/kernel-dev --target photospider_perlin_workflow -j 8
build/kernel-dev/examples/perlin_workflow/photospider_perlin_workflow whole 16384 4 5
build/kernel-dev/examples/perlin_workflow/photospider_perlin_workflow tiled 16384 4 5 128
build/kernel-dev/examples/perlin_workflow/photospider_perlin_workflow gpu 16384 4 5
```

The `gpu` mode uses the native backend selected when building the kernel. For a
Vulkan build, configure with `-DPHOTOSPIDER_ENABLE_VULKAN=ON`, then run the same
command to exercise the SPIR-V path. The FreeBSD Intel UHD 770 path has passed the
independent exact-bit comparison; NVIDIA and Linux Perlin Vulkan runs are pending.

Arguments are mode (`whole`, `tiled`, or `gpu`), sample count (1..65536), host workers
(1..64), measured repetitions (1..10000), and tile width (power of two, 1..4096).
Defaults are `tiled 16384 4 5 128`. The input is deterministic:
`((i*1709+719)%131071)/65536-1` for flattened coordinate element i.

An installed kernel package can build the example independently:

```sh
cmake -S examples/perlin_workflow -B build/perlin-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/gpu-install"
cmake --build build/perlin-consumer -j 8
build/perlin-consumer/photospider_perlin_workflow tiled 128 4 3 32
```

Each run compares output bytes with a separately executed one-worker Whole
reference. Mathematical validation is independent in
[the Fraction oracle runner](../../oracle/ops/generation/README.md).
The example uses the default Float64 output; all three registrations also accept
Float32 coordinates and the explicit String parameter `dtype="float32"`.

JSON includes median and nearest-rank p95 milliseconds, modeled peak host bytes,
issued work, computation callback count and observed simultaneous stage count.
There is one warmup. Timers include execution and copying to the same client byte
array in every mode; registry/context construction, graph compilation, input
construction, root-statistics queries and byte comparisons are outside timing.
Managed host capacity excludes client input/output arrays, allocator overhead,
thread stacks and OS memory. The diagnostics report actual activity and do not
promise every configured worker is simultaneously active for short workloads.

[Implementation and measured results](../../docs/built-in_ops/03-generation/perlin-implementation.md)
describe numerical bounds, resource behavior and supported scheduler scope.
