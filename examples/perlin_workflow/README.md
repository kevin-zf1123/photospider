# Strict Perlin CPU and GPU workflows

This example builds a public WorkflowDocument whose source is a Result containing one generic numeric tensor coordinates[N,3]. It binds that Result, executes one of the three registered operations, and reads output port values, whose Result tensor member is samples. The output is a generic numeric tensor with shape [N]; no image or color meaning is inferred.

Whole evaluates and validates the complete coordinate tensor. Tiled computes bounded regions and publishes them in order; its host tile scheduler runs the granted stage and the workflow checks complete output coverage. GPU mode requires a native Metal or Vulkan service and rejects CPU fallback. An affine input window preserves its backing owner and signed strides; the host materializes and accounts for a window when it is not one physical affine view.

    cmake --build build/kernel-dev --target photospider_perlin_workflow -j 8
    build/kernel-dev/examples/perlin_workflow/photospider_perlin_workflow whole 16384 4 5
    build/kernel-dev/examples/perlin_workflow/photospider_perlin_workflow tiled 16384 4 5 128
    build/kernel-dev/examples/perlin_workflow/photospider_perlin_workflow gpu 16384 4 5

Arguments are mode (whole, tiled, or gpu), sample count (1..65536), host workers (1..64), measured repetitions (1..10000), and tile width (power of two, 1..4096). Defaults are tiled 16384 4 5 128. Coordinates are deterministic: flattened element i is ((i*1709+719)%131071)/65536-1. This example uses Float64 coordinates and the default Float64 output.

The standalone project requires Photospider 0.30.0 and can build against an installed kernel package:

    cmake -S examples/perlin_workflow -B build/perlin-consumer \
      -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
    cmake --build build/perlin-consumer -j 8
    build/perlin-consumer/photospider_perlin_workflow tiled 128 4 3 32

Each run compares its output bytes with a separately executed one-worker Whole reference. The Fraction oracle checks a finite set of exact output bits; see the [oracle guide](../../oracle/ops/generation/README.md). All registrations accept Float32 or Float64 coordinates and the explicit String parameter dtype="float32" or dtype="float64".

JSON reports median and nearest-rank p95 latency, modeled peak host bytes, issued work, native dispatch and submission counts, native compute time, continuation_polls, and peak_active_tasks. continuation_polls is the sum of per-operation invocation_count diagnostics; it counts continuation polls, not computation callbacks. peak_active_tasks is a host diagnostic and does not report simultaneous CPU workers or tile stages. One warmup precedes measured repetitions. Timers include execution and copying into the same client-owned byte array in every mode; registry/context construction, compilation, input construction, statistics queries, and byte comparisons are outside the timer. Managed host capacity excludes client arrays, allocator overhead, thread stacks, and OS memory.

The GPU mode requires a native GPU service and does not fall back to CPU. If the service is unavailable, the example exits with code 77; CTest treats that code as a skip for both installed GPU consumer tests.

The standalone configure, build, and tiled run above pass against Photospider 0.30.0. Thirteen focused tests and six installed-consumer tests pass. The independent Fraction oracle passes 1,566 cases each for CPU Whole, CPU tiled, and native Metal GPU. Earlier FreeBSD Vulkan measurements and tests exercised the former Value interface and do not validate this Result path.

[Implementation and historical measurements](../../docs/built-in_ops/03-generation/perlin-implementation.md) describe current numerical/resource contracts and identify older execution measurements.
