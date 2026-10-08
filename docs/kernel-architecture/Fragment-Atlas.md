# Pack sparse fragments into an atlas

## Scope and ownership

The plan retains descriptor-domain metadata, exact sample coverage and occupied tile coordinates. Materialized payload and directory Values own packed bytes through the supplied allocator; a Result-backed atlas retains no source Result or read window. A native host keeps each device view alive until the synchronous invocation drains. Atlas support for all scalar widths is transport support only; it does not imply GPU arithmetic support for a dtype.

The atlas packs generic tensor sample bits into one payload and a sparse tile directory. Callers can prepare it from `ValueFragments` or from the exact coverage carried by a `ResultTensorInput` capability. The Result overload reads only that capability's authorized samples; it does not convert a Result to Value semantics or broaden input authorization. The older `ValueFragments` overload remains available to its existing callers: it rejects structural image metadata that requires `PlanarImage`, while accepted ColorArray tuples preserve their full-tuple rule. Atlas packing alone does not implement image operations or grant new reads.

## Core data structures and memory layout

```cpp
class ResultTensorInput;

struct FragmentAtlas final {
  ValueDescriptor descriptor;
  std::vector<std::uint64_t> tile_shape;
  std::uint64_t slot_count = 0, payload_bytes = 0;
  Value payload, directory;
  Result<std::uint64_t> address(
      const std::vector<std::uint64_t>& coordinate) const;
};

class FragmentAtlasPlan final {
 public:
  static Result<FragmentAtlasPlan> prepare(
      const ValueFragments& input, std::vector<std::uint64_t> tile_shape = {},
      const FootprintLimits& limits = {});
  static Result<FragmentAtlasPlan> prepare(
      const ResultTensorInput& input,
      std::vector<std::uint64_t> tile_shape = {},
      const FootprintLimits& limits = {});
  Result<FragmentAtlas> materialize(
      const ValueFragments& input, const BufferAllocator& allocator,
      const FootprintLimits& limits = {}) const;
  Result<FragmentAtlas> materialize(
      const ResultTensorInput& input, const BufferAllocator& allocator,
      const FootprintLimits& limits = {}) const;
};
```

`FragmentAtlasPlan` prepares transport metadata from exact coverage and materializes it through a caller-supplied allocator. It supports rank 1..8 and every built-in `ElementType` (UInt8, Int64, Float64, Float32, Int8, UInt16 and Int16) without converting sample bits. The plan retains domain, coverage and tile masks, but no source data owners. The `ResultTensorInput` overload acquires owning windows for the Need-authorized samples and copies them through `row_run()`. It supports signed strides, zero-stride broadcasts, fragmented windows and batch-prefixed spatial tensors without filling holes or bounding boxes. Once packed, the atlas remains valid independently of the input Result, poll and execution context.

`preparation_work()` reports the generic directory-planning work. `materialization_work()` reports the generic packing work. The `ResultTensorInput` overload precharges window acquisition and read-bound work through `FootprintLimits::consume_work`, so `materialization_work()` is not its complete charge. The older `ValueFragments` overloads preserve their caller-charged work convention; their `consume_work` callback is not invoked.

A `ResultTensorInput` may represent one source Result or private pieces created when a structured C2 producer's tensor Need is expanded into singleton-atom requests. Atlas preparation and materialization use only the capability's granted sample coverage. A compound capability has no single Result identity (`object_id() == 0`), but that value does not invalidate the authorized pieces or add read permission. The input windows retain their original Result owners while the atlas packs the requested samples; the atlas owns its packed buffers independently afterward. This path handles tensor Needs; it does not aggregate object or field support. Descriptor-only Empty C2 inputs are not supported.

The directory has a power-of-two slot count, at least two and at least twice the occupied tile count. Slots use open addressing with linear probing. Each slot is 80 little-endian bytes:

| Bytes | Meaning |
| --- | --- |
| 0..63 | Eight uint64 global tile coordinates; unused axes zero |
| 64..71 | uint64 valid-sample mask; zero means an empty slot |
| 72..79 | uint64 start byte offset in the packed payload |

The public `FragmentAtlas::address` and the C-compatible SDK macro `PS_FRAGMENT_ATLAS_MSL_V11` implement matching lookup. The latter defines `ps_atlas_address` for a Metal shader. The host supplies actual binding spans, slot count, shape, tile extents and payload byte count. Only payload and directory buffers are needed regardless of fragment count. Boundary mapping must occur in global coordinates before lookup. An absent mask bit/slot is an explicit missing sample, never a zero value or per-fragment clamp.

## Execution and resource state machine

The existing `DependencySession` uses the `ValueFragments` transport described below. The G4 C++ staged example in `main.cpp` and C Result plugins in `c_plugin.c` and `discovery_plugin.c` use Result tensor-Need atlas interfaces; the latter also uses Result GPU discovery, documented in [GPU Discovery](GPU-Discovery.md). The legacy dependency-service discovery protocol is documented separately there. Synchronous Result producers use their own exact rectangular or Need-authorized atlas access. Raw Float64/Int64 transport does not assert native floating-point arithmetic support.

### Staged GPU execution and resource state machine

A GPU `DependencySession::poll` requires complete `DependencyGpuServices` from its host. The Run uses the existing GPU worker, waiting admission, native device and synchronous `Invocation`; start/source callbacks retain the CPU worker. There is no second executor or shader page-fault mechanism. State-cache hits and control/constant-only paths may complete with zero new dispatches. Selected backend identifies the implementation/numeric contract; actual native work is reported separately in dispatch/submission/device-time counters and poll timings.

`phase.atlas(port)` lazily prepares and materializes only that stage's validated port coverage. It charges preparation and packing work before the host allocates and reuses the same atlas on repeated calls within a poll. Missing stage ports and CPU use fail. Native buffer acquisition and execution are fenced and sticky; ignored failures cannot publish success. The host keeps all acquired native views until the synchronous callback drains. Caller and auxiliary set cancellation are combined for ordinary, streamed and frozen dependency Runs; cancellation wins over ordinary callback/backend failures. An already detected Protocol violation, including an unauthorized native-buffer request, retains its Protocol status even when cancellation follows.

```text
prepare exact atlas -> reserve rounded payload and directory
        |                         |
        |                         +-> admission failure -> ResourceExhausted
        v
materialize -> synchronous native callback -> drain views -> retire leases
        |                    |
        +-> cancellation     +-> service/protocol failure -> no successful publish
```

Each atlas has a separate nonblocking MemoryBudget reservation for the exact native payload and directory capacities. Stage output bytes round each requested rectangle separately; declared workspace bounds count actual native capacity. Sealing releases only unused reservation bytes. Live atlases, state, scratch and outputs remain charged until their last owner retires. A stage that cannot fit with its retained owners fails finitely. Atlas leases never authorize additional input samples, batching or changes to per-output certificates.

The public [GPU fragment workflow](../../examples/g4_gpu_workflow/main.cpp) uses `ResultProgramNeed` for one Int64 control sample per output and 65 separated Float32 data samples per observation. Its native callback acquires a `ResultTensorInput` atlas and binds the packed payload and directory to the Metal shader. The independent sums are 2145, 4290 and 2145; two observations dispatch, and the third reuses a pure block with the same incoming state and data while preserving current Control evidence `{2}`. The shader explicitly checks for a missing sample before numerical publication. The completed output/state entries retain 36 native bytes.

`test_dependency_gpu` uses explicitly nonnative mocks for protocol-only service absence, CPU misuse, host exceptions, ignored errors, work-before-allocation and atlas reuse/retirement. It does not count as native evidence. The separate `test_gpu_fragment_execution` path exercises the Result workflow; it returns 77 on hosts without native Metal after checking the CPU oracle.

### Result callback access and Root ownership

`ResultProgramPhase::acquire_native_atlas(input, slot)` packs the exact coverage authorized by that callback's tensor Need. A missing Need, Descriptor-only Need, or CPU-phase call fails with sticky `InvalidArgument` and does not create an atlas. The method does not add sample authorization. Calls for the same port and slot within one poll reuse the same `shared_ptr<const FragmentAtlas>`.


An Empty payload Need returns a valid atlas with a one-byte inert payload and a two-slot empty directory. The operation reads no source samples and the empty lookup itself performs no native dispatch.

The atlas payload and directory use Root-accounted storage independent of operation workspace. The atlas retains those buffers, but no source Result or read window, so a caller-held atlas remains readable after its source and execution context retire. The native output producer still controls publication and must submit its dispatch using the current GPU service.

`main.cpp` also exercises reverse, zero-stride broadcast and fragmented input storage. A separate spatial case uses batch axes `{2,2}` and cell shape `{2,4}`; its masked Need returns 30 from a real Metal dispatch and leaves a requested hole absent. The test retains the atlas after source/context release and checks that its payload remains live until the atlas's final owner is released.

Failure cases cover a missing 65th sample (`OperationFailed`), CPU atlas access, Descriptor-only or missing-port access, immutable-input promotion, and a block callback that returns without dispatch. The protocol violations return sticky `InvalidArgument`; the other failed computation is `OperationFailed`. Each failure must retire its owners and permit a fresh native retry with one dispatch. Additional cases issue native service failures and exhaust work in both orders: the first observed error remains the reported status, so later work exhaustion cannot hide an earlier service error and a later service error cannot replace earlier work exhaustion. A normal cancellation returns `Cancelled`; an earlier unauthorized native-buffer request remains `InvalidArgument` with `UnauthorizedRead` and Protocol origin, and neither case publishes output.

The Result Root Payload frontier includes 98,304 bytes for the bound 8192-sample Float32 data and Int64 control tensors, a 152-byte `SparseState`, 4 bytes of output, 12-byte incoming and outgoing native block states, 8 bytes of scratch, a 260-byte sparse atlas payload, and a 32,768-byte directory. Thus 131,519 bytes is rejected and 131,520 bytes is admitted. The example releases output owners before repeating the admission check.

### C Result staged GPU bridge and handle lifetime

The C plugin receives `ps_result_services_v2` for each Result callback. Its native GPU service is the independently versioned `ps_gpu_service_v1` table declared by the standalone `photospider/plugin/native_gpu_api.h`; the enclosing Result operation interface remains ABI 2. `acquire_native_atlas` packs only the current tensor Need's authorized coverage. The returned `ps_result_native_atlas_v2` describes the full sample shape, tile shape, slot count, byte sizes, and payload/directory buffer tokens. Repeated acquisition of the same input and slot in one poll reuses the atlas while neither token has been released. Releasing either token retires the pair on the next acquisition, which returns fresh tokens; old tokens do not become valid again. The C adapter maps tokens to monotonically allocated handles and accepts them only in the poll that created them; stale, forged, or wrong-kind tokens fail before GPU dispatch. Need coverage remains capped at 65,536 boxes, and copying its region list precharges `count * (1 + 2 * rank)` Root work. Footprint and remaining work limits also bound atlas preparation and packing.

`ps_result_services_v2::block` supplies a restricted `ps_result_block_services_v2` table to the block callback. The incoming state is an explicit generic, single-tensor `CompleteBundle` Result handle. `create_state` copies typed sample bytes through the active Root allocator and seals a state Result; `read_state` copies all of its tensor samples; `release_state` retires the handle. The callback may return the borrowed incoming handle or a newly created handle. The host consumes a newly returned handle and gives the caller a distinct outgoing handle. Block callbacks may read input samples covered by the current Need and may use the atlas and GPU services. They cannot issue another Need, nest a block, or publish operation outputs.

`publish_tensor_buffer` accepts an exact scratch allocation and a row-major output region with relation rows. After validating its envelope, region, and relation, the service moves the allocation out of scratch and freezes it for publication. The allocation remains consumed if a later publication step fails, and its former writable scratch token cannot modify the transferred bytes. C GPU commands are bounded to 32 dispatches and 31 bindings per dispatch; token mappings and atlases live for the poll, while operation state handles live until released or consumed. Service errors remain sticky.

The C11 plugin `examples/g4_gpu_workflow/c_plugin.c` and loader `c_main.cpp` register a Result ABI 2 operation whose input is a rank-one Float32 tensor with at least 4,225 samples. The workflow returns `2145,2145` on CPU and on two native observations; the Metal path uses one native dispatch, one block-cache hit, and 16 bytes of retained native cache storage. The second observation records current Control evidence `{1}` and excludes obsolete control `{0}`. The fixture also exercises Empty output, dynamic input extents, state handle errors, excessive Needs, malformed atlas records, unauthorized promotion, writes after publication, shader misses, and poll-token lifetime. These are C Result bridge cases; the C discovery Result workflow is described in [GPU Discovery](GPU-Discovery.md), separately from legacy dependency-service discovery.

The standalone example CMake project builds this C11 module and loader. An optional loader argument selects a module path. Exit 77 means native Metal is unavailable after CPU checks; it is not native success. The installed consumer test `installed_gpu_c_abi_execution` validates the C Result loader against the installed kernel package and passes without a native-device skip.

### Synchronous producer execution and CPU fallback

Dependency Runs also invoke existing synchronous GPU producers on the context's native worker. The host collects each declared rectangular input demand into packed native storage, independently charges its rounded capacity, and supplies `ps_gpu_service_v1` from the standalone `native_gpu_api.h` for the callback lifetime. GPU callbacks must submit actual native work. Native invocation views drain before owners retire or CPU retry. Whole producers preserve their full-domain validation and evidence.

Only `BackendUnavailable` with both CPU support and `allows_cpu_fallback` permits retry. A staged failure retires the continuation and restarts the original Q on the CPU worker; it never polls GPU state with CPU services. Cancellation wins for the originating caller. A producer started from a frozen plan may continue for a healthy peer after the originating caller is cancelled or its Demand binding generation becomes stale; the originating caller still receives `Cancelled` or `Stale` at its final check. Borrowed-graph execution continues to enforce graph currentness. A frozen peer retains its captured input bundle, while a request against the latest Demand uses the replacement binding. Per-session limits apply to each attempt, and all attempts consume the bounded Run work. Diagnostics include actual backends and rejected attempt timings. Fallback ancestry propagates through child results, shared Flights and retained Whole records. Such results cannot populate dependency result caches, and their stages cannot use or retain checkpoints or pure blocks under native identities. Independent waiter cancellation still cannot abort a producer another waiter needs.

For atlas-backed staged GPU attempts, the `DependencySession` Run saves bounded record coverage before dispatch. CPU restart restores that coverage, including previously merged rows, and rebuilds local indexes so abandoned ancestors do not consume the retry's record allowance. The save/restore work is charged without refunding GPU discovery work. Shared immutable records, Flights and cache epochs are unchanged. Whole pixels and their complete structural DAG retain their at-most-once lifetime; their evidence is reimported only if the restarted path actually requests those pixels.

The synchronous Result example in [sync_main.cpp](../../examples/g4_gpu_workflow/sync_main.cpp) binds a 5000-sample Float32 input Result and requests `{0,2}`. It checks outputs 1 and 3 against an independent `x+1` oracle, verifies two real Metal dispatches, records the selected backend and attempt outcomes, and exercises missing-device CPU fallback. A fresh frozen execution reuses the completed pure Result with one cache hit and no dispatch; fallback-tainted results have zero cache hits.

The same Result workflow returns `BackendUnavailable` from Whole, staged-poll, and start-rejected GPU attempts. Eligible attempts restart on CPU, with the staged and start-rejected continuations each started twice. Four record-rollback cases run under 16-entry regional and 12-entry Whole limits. They verify that constant retries retain empty input support and no abandoned Tensor or Descriptor records, support previously recorded at `{0,2}` remains, and a Whole ancestor retains full 5000-sample support and is reused once when the CPU retry requests it. Dependency-record snapshot and restoration preserve typed evidence rather than only a coverage checkpoint.

The Whole allocation case keeps the 20000-byte source Result charged to the same Root as two native buffers, each rounded to 32768 bytes. Admission therefore fails at 85535 live bytes and succeeds at 85536. After releasing the first output, CPU fallback and a fresh native retry also fit the same limit. On hosts without Metal, missing-device fallback is checked before the example returns 77; this is a skip, not native evidence. `test_execution_demand` separately checks shared fallback with producer-waiter cancellation and zero cache retention. The C++ staged Result path is described above; the current C discovery workflow is Result-based as described in [GPU Discovery](GPU-Discovery.md). `ValueFragments` remains a typed-backing helper for atlas preparation and materialization, not an independent workflow or dependency-execution entry point.

## Algorithms and wire format

Each tile has at most 64 logical samples. Callers can specify positive rank-sized tile extents with product at most 64, or use the deterministic default geometry that fills the rightmost axes first. Only occupied tiles enter the directory. Global shape products need not fit uint64: lookup uses per-axis tile coordinates, never a flattened global index. `ValueFragments` rejects structural image facets that require `PlanarImage`; the atlas accepts generic Values and applies any accepted ColorArray complete-tuple rule without creating planar image execution support.

The hash is FNV-1a over the rank coordinate words, low byte first, with offset basis 14695981039346656037 and prime 1099511628211. Local row-major coordinates select a mask bit. A present sample's payload offset is the tile byte offset plus its preceding set-bit count times dtype width. Payload is ordered by tile key and increasing valid mask bit. No hole occupies a payload sample. An empty port uses one inert payload byte and a two-slot empty directory; neither authorizes samples.

`prepare` reports exact logical allocation sizes for payload and directory before materialization. A GPU host must round each separately to actual device capacity before admission. `maximum_boxes` bounds occupied tiles, while `maximum_work` bounds metadata scanning, sample discovery, hashing/probing and packing. Entry cancellation and metadata precharges precede cardinality/equality scans. `preparation_work()` reports generic geometry-planning work and `materialization_work()` reports generic packing work. The `ResultTensorInput` overload precharges its work through `FootprintLimits::consume_work`; Result materialization additionally precharges window acquisition and read-bound work that `materialization_work()` does not include. The `ValueFragments` overloads preserve the existing caller-charged convention. Cancellation after either allocation, including an empty input, retires all newly owned buffers and publishes no atlas.

## Limitations and failure handling

### Verification evidence

The installed consumer uses the public preparation/materialization/address APIs in [test_fragment_atlas.cpp](../../tests/unit/test_fragment_atlas.cpp). It checks four dtypes (UInt8, Int64, Float32 and Float64), exact wire words, holes including bit 63, changed input bits, negative strides, a rank-8 domain whose dense size exceeds uint64, precise allocation/work frontiers, source-owner retirement and cancellation during allocation. The native GPU test passes the same SDK helper to the real Metal service and compares returned flags and bits with an independent coordinate dictionary. The native run performs seven dispatches over ranks 1/3/8 and widths 1/4/8, including 65 separate fragments with four bindings, directory capacity rounding above 16 KiB, a one-byte-below native allocation failure, global out-of-domain queries and internal holes. Exit 77 means native Metal is unavailable and is a skip, not native success. These atlas-transport checks do not cover bounded GPU discovery; the current Result discovery behavior is described separately in [GPU Discovery](GPU-Discovery.md).

The `ValueFragments` overloads preserve samples in their accepted Value coverage; structural image facets requiring `PlanarImage` are rejected there. The `ResultTensorInput` overload preserves only samples authorized by its tensor Need. Both packers leave holes absent, return `NotFound` for a missing directory sample, and reject plans that exceed tile or work limits. Cancellation retires newly allocated owners. Device hosts must admit rounded payload and directory capacities separately; atlas lookup does not trigger additional reads or provide a CPU fallback.
