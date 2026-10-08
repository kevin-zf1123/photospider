# Dynamic graph scheduler benchmark

This benchmark exercises the public Result execution path. It has a scalar CPU chain and a mixed CPU/GPU workload containing Perlin, Gaussian, PixelOE, and dependency-sharing cases. Its counters describe scheduling and selected profiling scopes; they are not a scheduler-only benchmark or evidence of a performance improvement.

## Build and run

Build the examples in the Photospider source tree:

```sh
cmake --build build/kernel-dev --target photospider_dynamic_scheduler photospider_mixed_scheduler -j 8
cmake --build build/pixeloe/result-build --target photospider_pixeloe
build/kernel-dev/examples/dynamic_scheduler/photospider_dynamic_scheduler 1000 8 4 1
build/kernel-dev/examples/dynamic_scheduler/photospider_dynamic_scheduler 1000 8 4 8 1
build/kernel-dev/examples/dynamic_scheduler/photospider_mixed_scheduler \
  build/pixeloe/result-build/libphotospider_pixeloe.so metal 22 2 2 8 1024 1 0
```

An independent consumer can configure against a compatible Photospider 0.32 SDK and links only `Photospider::kernel`:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
cmake -S examples/dynamic_scheduler -B build/dynamic-scheduler-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/dynamic-scheduler-consumer \
  --target photospider_dynamic_scheduler photospider_mixed_scheduler -j 8
```

The standalone CMake project uses `find_package(Photospider 0.32 CONFIG REQUIRED COMPONENTS kernel)`. Its UNIX allocation targets are available when configured on a UNIX system. The private `photospider_dynamic_costs` and `photospider_mixed_transfers` targets require an in-tree Photospider build with `BUILD_TESTING=ON`; they are not part of the installed SDK consumer.

## Scalar Result chain

The scalar executable accepts measured requests, chain nodes, CPU workers, independent clients, and optional scheduler timing (`0` or `1`). Defaults are 1000 requests, 8 nodes, 4 workers, 1 client, and timing disabled. Bounds are 1..1,000,000 requests, 1..4096 nodes, 1..64 workers, and 1..min(64, requests) clients.

Each request creates a one-tensor Int64 Result source of shape `[1]`, binds that `ResultRef` to a new workflow, compiles a chain of `benchmark.increment` Result operations, and executes the named Result output. Each node requests its input tensor and publishes a new Result containing one tensor. The operation sequence is `start_result`, a `poll` that returns a Tensor Need, and a later `poll` that publishes the output: three scheduled callbacks per node when scheduler timing is enabled. The benchmark checks the final Int64 value against `input + nodes`; Result caching is disabled.

Clients share one frozen operation registry, compiler, and execution context. Each client runs 20 warmup requests before a barrier releases the measured phase. The `runs` requests are divided among clients. Construction, compilation, and `ExecutionContext::execute` durations are reported separately. Total per-request latency also includes reading and checking the output. `qps` is request count divided by summed total latency; `wall_qps` divides by the barrier-to-join wall interval, including measured-phase request teardown and thread joining. Per-request p50/p95/p99 include all measured requests. Warmup is excluded from interval deltas but included in the context-lifetime queue maxima.

With timing enabled, the executable snapshots cumulative CPU FIFO statistics around the measured phase. `cpu_callbacks`, `cpu_submission_ns`, and `cpu_queue_wait_ns` are measured deltas. Each successful callback adds three clock reads at submission entry, FIFO publication, and worker removal. Submission time ends at FIFO publication. Queue-wait time begins there and ends when a worker removes the callback; neither value measures operation execution or image transfer. Rejected submissions are excluded from the reported successful-callback counters. Range and staged-tile claims are outside these callback counts, though their use of workers can affect queue wait. The timing-disabled path reads no scheduler clocks.

## Mixed CPU/GPU Result workload

The mixed executable accepts a PixelOE module path, backend (`metal` or `vulkan`), measured attempts, CPU workers, clients, image side, waiting-queue limit, scheduler timing, and whether `ResourceExhausted` may count as an expected rejection. Each client warms up all 11 request kinds before measurement. Four deterministic seeds supply test data. A separate one-worker CPU Whole context computes differential references before the measured phase; those references are not independent mathematical oracles. Strict outputs are compared bitwise, PixelOE GPU outputs use an absolute tolerance of `1e-5`, and a GPU result that falls back to CPU fails validation.

Each input and output is a `ResultRef` with one tensor. Perlin uses Float64 coordinates, Gaussian uses Float64 grayscale tensors, and PixelOE uses the `photospider.image` schema with spatial HWC Float32 pixels, cell shape `[height, width, channels]`, and batch axes `{1,1}`. Its full sample shape is `[1,1,height,width,channels]`. The 11 request kinds cover Perlin Whole/tiled/GPU, Gaussian Whole/tiled/GPU, PixelOE Whole/tiled/GPU, a CPU Perlin → GPU Gaussian → GPU Gaussian → CPU Gaussian chain, and three independent Perlin branches followed by a verification join.

The join reads all three Float64 branch tensors and checks their bits element by element. Its dependency support is Whole and Conservative. It publishes an identity tensor view of the first branch, so the output reuses that Result's storage rather than copying the pixel payload.

For the Perlin and Gaussian tiled cases, an `ExecutionOptions::result_publication` observer accumulates newly published coverage boxes, copies those boxes into a comparison buffer, rejects overlap or out-of-range geometry, and verifies complete coverage against the final Result. JSON keeps the `stream_output_tiles` counter and the `execute_scope` label containing `stream_checks`; here the count means observed publication boxes and the checks validate those publications. The benchmark does not call the public `execute_stream` API, and the count is not a scheduler tile-task count. PixelOE CPU tiled execution is checked through its Result output and diagnostics.

`attempt_qps` and `success_qps` divide attempt and successful-request counts by the measured wall interval, which includes result verification, cleanup, and joining client threads. Per-kind latency is recorded for every attempt before verification; native dispatch, submission, device, transfer, and other diagnostics are summed over successful requests only. `transfer_count` and `transfer_bytes` are counts, not transfer duration. FIFO and resource peaks include warmup, while issued-work deltas cover the measured interval. Concurrent Runs share context-level scheduler snapshots, so these fields do not isolate a single request or prove CPU/GPU overlap.

## Optional profiles

The allocation profiles replace global C++ `new`/`new[]`, including aligned and nothrow forms. Build them on UNIX with:

```sh
cmake --build build/kernel-dev --target photospider_dynamic_allocations photospider_mixed_allocations -j 8
build/kernel-dev/examples/dynamic_scheduler/photospider_dynamic_allocations 1000 8 4 1 0
build/kernel-dev/examples/dynamic_scheduler/photospider_mixed_allocations \
  build/pixeloe/result-build/libphotospider_pixeloe.so metal 22 2 2 8 1024 1 0
```

The allocation JSON counts successful routed C++ allocation returns, requested bytes, and failures across all client threads during the measured barrier-to-join interval. Per-attempt fields divide those process-wide totals by measured requests; they do not attribute allocations to individual concurrent requests. Direct C allocation, driver-private allocation, and DSO allocations that bypass the replacement are not counted. These are neither live bytes nor peak memory or RSS. `timings_are_profiled` is true because shared atomic counters add work.

In an in-tree build with `BUILD_TESTING=ON`, the private `photospider_dynamic_costs` and `photospider_mixed_transfers` targets are also available. They use the noninstalled test kernel. The PixelOE I/O profile module is built separately:

```sh
cmake --build build/kernel-dev --target photospider_dynamic_costs photospider_mixed_transfers -j 8
cmake --build build/pixeloe/result-build --target photospider_pixeloe_io_profile -j 8
build/kernel-dev/examples/dynamic_scheduler/photospider_mixed_transfers \
  build/pixeloe/result-build/libphotospider_pixeloe_io_profile.so metal 22 2 2 8 1024 1 0 \
  2>build/kernel-dev/scheduler-copy-profile.log
```

The profile plugin emits `PS_PIXELOE_IO` JSON lines on standard error. Its input interval covers creation and filling of the PixelOE input image through the end of input span processing; its output interval covers result selection and publication after PixelOE computation. These are host-side intervals, not pure `memcpy` time or device DMA time. `PS_COPY_PROFILE_BEGIN` and `PS_COPY_PROFILE_END` delimit the measured interval in the transfer-profile log; filter PixelOE records to those markers because references and warmups can also emit them.

The private `copy_profile` JSON reports `value_materialization`, `native_gather`, and `atlas_materialization` calls, logical bytes, host time, and failures across the measured interval. These hook scopes are not a complete copy inventory for structured Result execution; a zero counter does not prove that the Result path performed no copies. The `execution_stages` fields (`ready_dispatch`, `completion_publication`, and `final_assembly`) instrument `ExecutionRun` only. They do not measure internal `StructuredExecution` stages, so zero values for this Result workload are not evidence that those stages did no work. Instrumentation adds clocks and synchronization; use these profiles to inspect their named scopes, not to claim scheduler-only performance or a speedup.

## Validation recorded

The scalar local and installed-SDK smoke runs completed 12 requests with three nodes and scheduler timing enabled; the measured interval reported the expected 108 CPU callbacks (`12 × 3 × 3`). The mixed local and installed-SDK Metal smoke completed 22 attempts at side 8 with two workers and two clients; each request kind succeeded twice, with 68 native dispatches, 62 native submissions, and 16 observed publication boxes. These are execution and accounting checks, not performance results. All four in-tree profile commands exited successfully; the two allocation profiles reported zero allocation failures. The transfer smoke emitted the expected PixelOE I/O records and a nonzero `value_materialization` count; its zero `execution_stages` counters reflect their `ExecutionRun`-only scope. Vulkan was not covered by these mixed smoke checks, and no full benchmark matrix or speed comparison is claimed.
