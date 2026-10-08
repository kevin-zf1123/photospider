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

An independent consumer can configure against a compatible Photospider 0.33 SDK and links only `Photospider::kernel`:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
cmake -S examples/dynamic_scheduler -B build/dynamic-scheduler-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/dynamic-scheduler-consumer \
  --target photospider_dynamic_scheduler photospider_mixed_scheduler -j 8
```

The standalone CMake project uses `find_package(Photospider 0.33 CONFIG REQUIRED COMPONENTS kernel)`. Its UNIX allocation targets are available when configured on a UNIX system. The private `photospider_dynamic_costs` and `photospider_mixed_transfers` targets require an in-tree Photospider build with `BUILD_TESTING=ON`; they are not part of the installed SDK consumer.

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
