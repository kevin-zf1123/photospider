# G4 GPU workflows

This directory contains GPU workflows built on the public Result API. The C++ fragment-atlas path in `main.cpp`, synchronous fallback path in `sync_main.cpp`, and both C plugins in `c_plugin.c` and `discovery_plugin.c` use Result operations. `PS_GPU_DISCOVERY_MSL_V11` remains available for existing legacy dependency-service consumers; the discovery example uses the Result v2 record format and `PS_RESULT_GPU_DISCOVERY_MSL_V2`.

The fragment-atlas C++ example in `main.cpp` registers a staged Result operation, compiles a workflow and executes exact requests through `ExecutionContext::execute_fragments`. For each output, the callback first requests one Int64 Control sample, then requests the 65 separated Float32 samples selected by that value. On the native path it packs only that Need-authorized coverage with `ResultProgramPhase::acquire_native_atlas` and binds the payload and directory for Metal lookup.

Expected results are `2145,4290,2145`: two native dispatches and one pure block cache hit. The third observation retains Control evidence for coordinate `{2}` even though it reuses the computation for the earlier matching state and data. Native cached output and state storage totals 36 bytes. A fresh Result query for output 2 also reuses the immutable native block with zero dispatches and one block-cache hit. The native block cache accepts an entry only after the compute callback has dispatched native work; a synthetic block state without a dispatch cannot prime it.

The example checks a 65-sample request whose final sample is missing, CPU atlas access, Descriptor-only and missing-port atlas requests, unauthorized native-buffer promotion, and a block callback that returns without dispatching. Missing data fails with `OperationFailed`; unauthorized access fails with sticky `InvalidArgument`. Each failed path releases its owners and is followed by a fresh native retry. Additional cases inject native service failures and work exhaustion in both orders: the first error remains reported, so later work exhaustion cannot mask an earlier service error, and a later service error cannot replace earlier work exhaustion. Ordinary cancellation returns `Cancelled`; an earlier unauthorized buffer request keeps `InvalidArgument`, `UnauthorizedRead`, and Protocol origin even when cancellation follows. Neither failure publishes output.

Additional inputs exercise reversed, zero-stride broadcast, and fragmented storage. A spatial Result uses batch axes `{2,2}` and cell shape `{2,4}`; its masked Need returns 30 on Metal and leaves an unrequested sample absent. A retained atlas remains readable after its source Result and execution context retire, then releases its Root Payload on the last atlas owner.

The native Root Payload admission frontier is 131520 bytes: the bound source tensors use 98304 bytes, sparse state uses 152, output uses 4, incoming and outgoing native states use 12 bytes each, scratch uses 8, the sparse atlas payload uses 260, and the directory uses 32768. Admission fails at 131519 and succeeds at 131520; releasing the prior output allows the native retry to fit the same limit.

Build in the repository:

```sh
cmake --build build --target test_gpu_fragment_execution test_gpu_sync_fallback -j 8
build/test_gpu_fragment_execution
build/test_gpu_sync_fallback
```

Or build against a compatible installed Photospider package:

```sh
cmake -S examples/g4_gpu_workflow -B build/g4-gpu-consumer -DCMAKE_PREFIX_PATH=/path/to/photospider/install
cmake --build build/g4-gpu-consumer -j 8
build/g4-gpu-consumer/test_gpu_fragment_execution
build/g4-gpu-consumer/test_gpu_sync_fallback
```

Exit 77 means native Metal is unavailable; the CPU oracle still runs. It does not count as native success. The staged C Result bridge is described below; the C discovery example below also uses Result ABI 2.


The same project builds a C11 plugin and its public workflow loader:

```sh
cmake --build build --target test_gpu_c_abi_execution -j 8
build/test_gpu_c_abi_execution
# Standalone installed-package build also includes these targets:
build/g4-gpu-consumer/test_gpu_c_abi_execution
```

The C module registers a Result ABI 2 operation. Its input is one rank-one Float32 tensor with at least 4225 samples; the workflow binds and reads Result tensors and publishes a Result tensor. The CPU path returns `2145,2145`; the Metal path also returns `2145,2145` across two observations, with one native dispatch, one block-cache hit and 16 bytes of retained native cache storage. A repeated same-poll atlas acquisition reuses its payload and directory tokens. Releasing either token invalidates the pair; a later acquisition creates a fresh pair. Tokens resolve only within the poll that created them, and the C adapter validates token kind and binding spans before forwarding GPU commands.

The block callback receives a restricted Result service table. It can read samples authorized by the current Need, read or create a generic one-tensor `CompleteBundle` state, acquire a Need-scoped atlas, use scratch and GPU services, and return either its borrowed incoming state or a new state handle. The host consumes a new returned handle and creates a distinct outgoing handle. Successful `publish_tensor_buffer` transfers the exact scratch allocation into the Result tensor and freezes it, ending writes through its former writable token. Block callbacks cannot issue another block or publish an operation output.

The C behavior path also exercises stale and forged tokens, malformed native records, unauthorized input promotion, writes after publication, missing shader samples, invalid or repeated state handles, oversized Needs, and invalid block-service use. Failure-path computation runs with Result caching disabled so retained owners must retire before a fresh attempt. An optional argument selects the module path. The configured installed consumer builds the C loader and runs `installed_gpu_c_abi_execution`:

```sh
cmake --build build/consumer-build --target test_gpu_c_abi_execution -j 8
ctest --test-dir build/consumer-build -R '^installed_gpu_c_abi_execution$' --output-on-failure
```

The current local and installed C ABI execution checks pass; the installed check ran without a native-device skip.


The discovery C11 module and loader exercise Result GPU discovery followed by host Need supply and native computation:

```sh
cmake --build build --target test_gpu_discovery_workflow -j 8
build/test_gpu_discovery_workflow
# Also available in the standalone package consumer build:
build/g4-gpu-consumer/test_gpu_discovery_workflow
```

The Result operation binds rank-one Float32 data and Int64 control tensors. CPU and Metal execution return `8,24`; Metal submits four native dispatches. The discovery shader reads an already supplied Control atlas and emits the additional data rectangles. The host validates and groups them by input, tensor slot and roles, then attaches them to the operation's next tensor Need. A control edit changes the first value to 24 and replaces its data support with `{1,4097}`; the frozen execution still returns 8. The workflow rejects an invalid control value, table overflow, premature publication and `maximum_gpu_requests=0`.

The table uses Root-accounted native storage outside the operation workspace. For a capacity of 128, the example's payload admission boundary is 132,400 bytes: 98,304 bytes of bound source tensors, 1,160 bytes of continuation state, 32,768 bytes of native table capacity, and 168 bytes for the Control atlas. The boundary test rejects 132,399 bytes and admits 132,400 bytes. This result is specific to this example's shapes and device capacity. The Result v2 wire format, receipt lifecycle, Need grouping, and legacy v11 boundary are documented in [GPU Discovery](../../docs/kernel-architecture/GPU-Discovery.md).

`test_gpu_discovery_workflow` uses the real Metal workflow with rank-one inputs at tensor slot 0. It checks that `discover` runs on the callback entry thread. In the C++ phase probe, two workers concurrently charge 4,096 units each through `ResultProgramPhase::consume_work`; C discovery services remain entry-thread-only. The 8,512-unit limit fails and 8,513 admits one dispatch. Separate discovery-work and Run-work cases each admit a one-sample request at 8,192 units but reject a two-sample request when later polling exceeds that cumulative budget. The C callback retains its receipt handle until the resumed poll copies the records and releases it. The host freezes the request table after discovery; a later write through its GPU token fails. Failed runs return table and atlas owners to the source-only Root level and permit a fresh native retry.

`test_result_gpu_discovery` covers decoder cases beyond the example's native shape: rank-8 coordinates, tensor slot 1, batch/channel closure, and mixed role groups. These decoder inputs are not the rank-one, slot-zero Metal workflow.

Build and run the local decoder and native workflow checks:

```sh
cmake --build build --target test_result_gpu_discovery test_gpu_discovery_workflow -j 8
ctest --test-dir build -R '^(test_result_gpu_discovery|test_gpu_discovery_workflow)$' --output-on-failure
```

In the configured installed consumer, build and run the native workflow with:

```sh
cmake --build build/consumer-build --target test_gpu_discovery_workflow -j 8
ctest --test-dir build/consumer-build -R '^installed_gpu_discovery_workflow$' --output-on-failure
```


## Synchronous GPU fallback with Result

`sync_main.cpp` and `result_support.hpp` use the public Result workflow API. The example binds a 5000-sample Float32 input Result, requests only samples `{0,2}`, and checks outputs 1 and 3 against an independent `x+1` oracle. The staged Result operation first issues a tensor Need and its CPU retry publishes a mapped Result view. With native Metal, the request uses two dispatches. The executable records the selected backend and each attempt. A fresh frozen execution reuses the completed pure result with one cache hit and no dispatch; a fallback-tainted result has zero cache hits and is recomputed.

The example also exercises `BackendUnavailable` during Whole, staged-poll, and start-rejected attempts. CPU restart discards state from the failed attempt and preserves only valid dependency evidence. The staged and start-rejected continuations each start twice, both with Metal enabled and in the missing-device path. Four rollback cases check that constant retries retain empty input support and no abandoned Tensor or Descriptor records, previously completed support at `{0,2}` remains intact, and a Whole ancestor retains full support over 5000 samples and is reused once when the CPU retry needs it. Regional and Whole rollback cases run under 16-entry and 12-entry record bounds.

The Whole case also checks native resource admission. A 20000-byte source Result remains charged to the same Root; the native source and destination allocations each round to 32768 bytes. The combined live total is 85536 bytes, so 85535 fails and 85536 succeeds. Releasing the first output permits a fallback attempt and a later native retry within the same limit.

Build and run the Result path in the repository:

```sh
cmake --build build --target test_gpu_sync_fallback -j 8
build/test_gpu_sync_fallback
ctest --test-dir build -R '^test_gpu_sync_fallback$' --output-on-failure
```

To build the same Result example against the installed kernel package from the configured consumer project:

```sh
cmake --install build --prefix "$PWD/build/consumer-install"
cmake -S tests/consumer -B build/consumer-build \
  -DCMAKE_PREFIX_PATH="$PWD/build/consumer-install"
cmake --build build/consumer-build \
  --target test_gpu_fragment_execution test_gpu_sync_fallback -j 8
ctest --test-dir build/consumer-build \
  -R '^(installed_gpu_fragment_execution|installed_gpu_sync_fallback)$' --output-on-failure
```

On a host without native Metal, the executable checks missing-device CPU fallback and then returns 77. That skip does not count as native evidence. The test target is `test_gpu_sync_fallback`; its behavior is also described in [Fragment Atlas](../../docs/kernel-architecture/Fragment-Atlas.md#synchronous-producer-execution-and-cpu-fallback).

The `main.cpp` atlas example above also uses Result. Both staged C and C discovery callbacks use Result ABI 2; legacy v11 discovery transport remains documented separately for existing dependency-service consumers.
