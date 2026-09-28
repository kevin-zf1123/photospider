# Dynamic graph scheduler benchmark

Build and run through the public kernel API:

```sh
cmake --build build/kernel-dev --target photospider_dynamic_scheduler -j 8
build/kernel-dev/examples/dynamic_scheduler/photospider_dynamic_scheduler 1000 8 4 1
build/kernel-dev/examples/dynamic_scheduler/photospider_dynamic_scheduler 1000 8 4 8
build/kernel-dev/examples/dynamic_scheduler/photospider_dynamic_scheduler 1000 8 4 8 1
```

Arguments are measured requests, chain nodes, context CPU workers, independent clients, and optional scheduler timing (`0` or `1`). The fourth argument defaults to one and accepts `1..min(64,runs)`; the fifth defaults to `0`. All clients share one frozen operation registry, `Compiler` and `ExecutionContext`; each client first submits 20 warmup requests. The JSON field `warmup_per_client` reports that per-client count; aggregate warmup requests equal `clients * warmup_per_client`. A barrier releases the measured phase after every warmup completes. The total `runs` requests are distributed across clients, and each request creates a new `WorkflowDocument`, `GraphContext`, binding and plan. Every result is checked against an independent Int64 oracle requiring `output = input + nodes`. Result caching is disabled.

The `qps` field divides request count by summed per-request latency, so it is the inverse mean latency and remains a latency statistic when several clients run concurrently. `wall_qps` divides the same measured request count by elapsed time from barrier release through completion and joining of every client thread. It includes request teardown, barrier release and thread joining, and excludes per-client warmup. Use `wall_qps` to compare aggregate concurrent throughput. Construction, compilation and execution p50 are reported separately; total p50/p95/p99 cover every measured request across clients. Each client sample vector reserves capacity before thread launch. After joining, the main thread reserves aggregate arrays and computes percentiles.

With the fifth argument set to `1`, `collect_scheduler_timing` enables cumulative callback FIFO observations on the context. The benchmark snapshots before the measured barrier release and after client join, then reports measured CPU callback count, `cpu_submission_ns` and `cpu_queue_wait_ns` deltas. `cpu_queue_wait_max_ns_including_warmup` and `cpu_queued_peak_including_warmup` are context-lifetime maxima and include warmup; do not subtract them as interval statistics. The default `0` path reads no scheduler clocks and returns zero counters. When enabled, each successful callback uses three clock reads: submit entry, FIFO publication and worker removal. A rejected submission still incurs the submit-entry read but is excluded from the counters. `submission_ns` ends at the publication timestamp taken after FIFO insertion; it excludes the later notification and mutex unlock before `submit` returns. `queue_wait_ns` runs from that timestamp through worker removal, including notification, mutex release and the worker's subsequent wait to take the pool lock. Neither field is image transfer time or operation execution time. Range and staged-tile claims are outside these callback counters, though their worker use can increase callback queue wait. Shared-context counters include concurrent work from every Run using that context.

The benchmark remains a scalar CPU operation chain. It measures neither mixed CPU/GPU work nor Whole range scaling. The worker/client matrix has been smoke-run with 1 and 8 clients at 1 and 4 kernel workers on macOS and FreeBSD; those runs establish that the configurations execute, not a performance gain. Run without concurrent builds or profilers for a controlled comparison.

Build every benchmark against headers and a library from the same source version. Record the source identity, build paths, compiler settings and exact commands for each variant; do not compile a current benchmark against an older library whose public header lacks the timing API. In the recorded FIFO timing comparison, `before` came from the source immediately preceding the timing change and its matching headers/library, not from the supplied input snapshot. Keep raw JSON, compiler version, hardware and worker count with the results. Profiler runs and allocation/syscall traces are separate from final timing runs.

## C++ allocation profile

UNIX builds define separate `EXCLUDE_FROM_ALL` profile targets. The ordinary benchmark executables continue to use the process allocator. Build and run the profile variants explicitly:

```sh
cmake --build build/kernel-dev --target photospider_dynamic_allocations photospider_mixed_allocations -j 8
build/kernel-dev/examples/dynamic_scheduler/photospider_dynamic_allocations 20000 8 4 8 0
build/kernel-dev/examples/dynamic_scheduler/photospider_mixed_allocations \
  build/pixeloe/libphotospider_pixeloe.so vulkan 110 4 4 8 1024 1 0
```

The scalar profile accepts the same arguments as the ordinary scalar executable; the mixed profile accepts the same arguments as the ordinary mixed executable. The profile targets replace global C++ `new`/`new[]`, including aligned and nothrow forms. They count successful routed returns and their requested bytes, plus allocation failures, over the measured barrier-to-join interval. JSON `calls_per_attempt` and `requested_bytes_per_attempt` divide process-wide totals by measured attempts; they do not attribute counts to individual overlapping requests. Direct C allocation, driver-private allocation and DSO heap traffic that bypasses the replacement are outside the counters. These values are not live bytes, peak memory or RSS.

The profile sets `timings_are_profiled=true`; shared atomic counter updates perturb execution. Use the profile to inspect allocation volume, not to claim a speedup. The recorded scalar profile had zero allocation failures and reported 1,240 calls / 151,448 requested bytes per attempt on macOS and 1,238 / 150,584 on FreeBSD, for both one- and eight-client runs. These are process-wide per-attempt averages, not per-request counts. Focused profile tests passed 3/3 on macOS and 2/2 on FreeBSD; see [the detailed cost model](../../out/gpu-whole-tiled/SCHEDULER_COST_MODEL.md).

## Host copy and PixelOE I/O timing

With `BUILD_TESTING=ON`, a separate `photospider_mixed_transfers` executable links the noninstalled test kernel and a private copy observer. PixelOE has a separate `photospider_pixeloe_io_profile` plugin. Neither replaces the ordinary mixed benchmark or installed PixelOE plugin.

```sh
cmake --build build/kernel-dev --target photospider_dynamic_costs photospider_mixed_transfers -j 8
cmake --build build/pixeloe --target photospider_pixeloe_io_profile -j 8
build/kernel-dev/examples/dynamic_scheduler/photospider_mixed_transfers \
  build/pixeloe/libphotospider_pixeloe_io_profile.so vulkan 110 4 4 8 1024 1 0 \
  2>build/kernel-dev/scheduler-copy-profile.log
```

The kernel observer reports host time for Value materialization (allocation, copy and metadata publication), native fragment gathering and fragment-atlas materialization, with successful byte counts and separate failure counts. The profile PixelOE plugin writes a successful-path `PS_PIXELOE_IO` JSON record to standard error with `profile`, `input_bytes`, `input_host_ns`, `output_bytes` and `output_host_ns`; the ordinary installed plugin has no such timers or output. Its input/output intervals include row access, domain checks, work charges and copies; CPU tiled intervals also include stage submission and join. They are neither pure `memcpy` time nor device DMA time. GPU copy kernels remain visible in `native_compute_us`, and shared host access remains visible in `host_access_count`.

The transfer profile writes `PS_COPY_PROFILE_BEGIN` and `PS_COPY_PROFILE_END` to standard error around the measured barrier-to-join window. Parse PixelOE records only between those markers; the plugin also writes records for successful CPU Whole references and warmups outside the window. The private kernel copy observer starts before references and warmups, but it only stores events between the markers. The mixed transfer JSON marks `timings_are_profiled=true`; the PixelOE stderr line does not repeat this field. Both instruments add clocks and synchronization, so their durations explain host cost and do not support performance-improvement claims. The recorded copy samples were collected before the `ExecutionRun` stage measurements were added; they describe that earlier build variant. The copy collection gates passed on macOS and FreeBSD for side 8/110 requests and side 32/44 requests. The host durations are event intervals that can overlap across callbacks; do not sum them as wall time or add them directly to queue-wait totals. Output spans can include noncompact padding. See [the measured cost model](../../out/gpu-whole-tiled/SCHEDULER_COST_MODEL.md).

`photospider_dynamic_costs` also emits private `execution_stages` measurements for `ready_dispatch`, `completion_publication` and `final_assembly`. These cover scalar `ExecutionRun` internals: ready-step removal through submission handling, completion handling through publication, and output-view/diagnostic assembly. Final assembly stops before Run-held Values and bindings are cleared. Three-run samples on macOS and FreeBSD recorded all expected counts with zero failures; their median per-event means and ranges are in the [cost model](../../out/gpu-whole-tiled/SCHEDULER_COST_MODEL.md). Final mixed copy-profile records also contain these observations for Run paths, but they do not cover CPU_STAGES, dependency or tiled execution end-to-end. Concurrent Run scopes can overlap, and instrumentation adds clocks and observer work, so event durations are not additive wall time or production speed estimates.

## Mixed CPU/GPU benchmark

The separate mixed executable uses one frozen registry, compiler and execution context shared by its client threads. It rotates eleven request kinds: the CPU Whole, CPU tiled and GPU forms of Perlin, Gaussian and PixelOE; a Perlin → GPU Gaussian → GPU Gaussian → CPU Gaussian chain; and three Perlin branches feeding a verification join. It streams actual Perlin/Gaussian tiles and checks PixelOE CPU staged execution. Build and run it with the installed PixelOE plugin:

```sh
cmake --build build/kernel-dev --target photospider_mixed_scheduler -j 8
build/kernel-dev/examples/dynamic_scheduler/photospider_mixed_scheduler \
  build/pixeloe/libphotospider_pixeloe.so metal 110 4 4 8 1024 1 0
build/kernel-dev/examples/dynamic_scheduler/photospider_mixed_scheduler \
  build/pixeloe/libphotospider_pixeloe.so metal 110 4 8 8 1 1 1
```

The argument order is plugin path, backend (`metal` or `vulkan`), measured attempts, CPU workers, clients, image side, waiting-queue limit, scheduler timing (`0` or `1`) and whether `ResourceExhausted` may count as an expected rejected attempt (`0` or `1`). Each client warms up all eleven kinds before the measured phase. Before measurement, a separate one-worker CPU context runs the corresponding CPU Whole forms to produce per-kind differential reference outputs; these references are not an independent mathematical oracle. Strict operator results are compared bitwise, while PixelOE GPU uses its declared `1e-5` tolerance. A GPU sample is rejected if it falls back to CPU.

`attempt_qps` and `success_qps` divide their respective request counts by the measured wall interval, which includes output verification, cleanup and joining. Per-kind latency is recorded for all attempts before verification; per-request dispatch, device and transfer diagnostics sum successful requests only. Scheduler count and duration deltas cover measured attempts, while FIFO maxima and managed-capacity peaks include warmup. `transfer_count` and `transfer_bytes` report counts and bytes, not elapsed transfer time; the benchmark does not provide `transfer_us`. CPU/GPU scheduler snapshots share a context with concurrent Runs. A single run establishes neither stable QPS nor measured CPU/GPU overlap.
