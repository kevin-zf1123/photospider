# Pack sparse fragments into an atlas

## Scope and ownership

The plan retains descriptor-domain metadata, exact coverage and occupied tile coordinates. Materialized payload and directory Values own packed bytes through the supplied allocator; they retain no source Value owner. A native host keeps each device view alive until the synchronous invocation drains. Atlas support for all scalar widths is transport support only; it does not imply GPU arithmetic support for a dtype.

The atlas is a transport for generic Value fragments. The Value fragment constructor rejects structural image metadata that requires `PlanarImage`; accepted ColorArray tuples preserve their full-tuple rule. This transport does not make image-v2 dependency execution available.

## Core data structures and memory layout

```cpp
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
  Result<FragmentAtlas> materialize(
      const ValueFragments& input, const BufferAllocator& allocator,
      const FootprintLimits& limits = {}) const;
};
```

`FragmentAtlasPlan` prepares transport metadata from an exact `ValueFragments` coverage, then materializes that plan through a caller-supplied allocator. It supports rank 1..8 and every built-in `ElementType` value (UInt8, Int64, Float64, Float32, Int8, UInt16 and Int16) without converting sample bits. The plan retains domain/coverage and tile masks, with no source Value/storage owners. Materialization reads the current matching input, including negative strides and separate physical fragments. It never fills a bounding-box gap.

The directory has a power-of-two slot count, at least two and at least twice the occupied tile count. Slots use open addressing with linear probing. Each slot is 80 little-endian bytes:

| Bytes | Meaning |
| --- | --- |
| 0..63 | Eight uint64 global tile coordinates; unused axes zero |
| 64..71 | uint64 valid-sample mask; zero means an empty slot |
| 72..79 | uint64 start byte offset in the packed payload |

The public `FragmentAtlas::address` and the C-compatible SDK macro `PS_FRAGMENT_ATLAS_MSL_V11` implement matching lookup. The latter defines `ps_atlas_address` for a Metal shader. The host supplies actual binding spans, slot count, shape, tile extents and payload byte count. Only payload and directory buffers are needed regardless of fragment count. Boundary mapping must occur in global coordinates before lookup. An absent mask bit/slot is an explicit missing sample, never a zero value or per-fragment clamp.

## Execution and resource state machine

C++ staged GPU Runs integrate this transport as described below. Bounded GPU discovery is integrated through [bounded request tables](GPU-Discovery.md); the C staged bridge shares this transport. Synchronous GPU producers use the exact rectangular transport described below. Raw Float64/Int64 transport does not assert native floating-point arithmetic support.

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

The public [GPU fragment workflow](../../examples/g4_gpu_workflow/main.cpp) reads one Int64 control per observation on the host, declares 65 isolated Float32 samples, and performs a real Metal atlas sum with three bindings. Independent arithmetic requires results 2145, 4290 and 2145. Two observations dispatch; the third reuses a pure block with identical incoming control/state and currently supplied data, while preserving its own control `{2}` evidence. The shader explicitly checks missing samples before numerical publication. Additional runs verify the exact 33079/33080-byte failed/successful stage reservation frontier and owner reuse, plus ordinary streaming cancellation and Protocol-before-cancellation priority.

`test_dependency_gpu` uses explicitly nonnative mocks for protocol-only service absence, CPU misuse, host exceptions, ignored errors, work-before-allocation and atlas reuse/retirement. It does not count as native evidence. Installed static and shared consumers run that test and compile the same public workflow. The workflow returns 77 on hosts without native Metal, after verifying its CPU oracle.

### C staged GPU bridge and handle lifetime

Both `ps_dependency_services_v11` and `ps_dependency_block_services_v11` expose `atlas`, `gpu_buffer` and `gpu_execute`, with Boolean 1 success / 0 failure. This return convention differs from `ps_gpu_service_v11` result codes. A `ps_dependency_atlas_v11` reports full-domain shape/tile geometry, slot count, valid sample bytes, physical binding spans and immutable payload/directory view tokens. It exposes no new source reads. Repeated same-port lookup returns the same tokens within a poll. CPU use fails with InvalidArgument.

The adapter maps native view tokens to invocation-monotonic C handles. Only the current poll's map can translate dispatch bindings; previous-poll, forged or other-kind handles cannot accidentally refer to a newly reused native token. Output publication revokes writable access even for an already acquired token. The C adapter bounds command count to 32, bindings to 31 per command and views to 1024 per poll, and charges command/binding metadata before copying. Native validation still enforces source, constants, access and grid bounds. Every service error is sticky, including ignored errors inside pure compute. Atlas and native view owners survive synchronous drain, then retire with their normal Phase/Invocation leases.

The public C11 plugin `examples/g4_gpu_workflow/c_plugin.c` and loader `c_main.cpp` run a real two-observation workflow. Each host control read selects 65 isolated Float32 samples. The expected sum is 2145 for both observations; there is one actual Metal dispatch and one block hit. The second observation retains current control `{1}` and excludes obsolete control `{0}`. Retained native output/state capacity is 16 bytes. Negative runs cover stale/forged handles, malformed atlas records, invalid view/command/binding arguments, immutable input promotion, writes after publication and actual shader misses. A missing sample returns failure before numerical output publication. Each failed case is followed by a real uncached retry in the same tight-budget context, so an old cached output cannot hide retained failure-path allocations.

The standalone example CMake project and static/shared installed consumers also build this C11 module and its public loader. An optional loader argument selects a module path. Exit 77 means native unavailable after CPU checks, never native success. The bridge also exposes [bounded discovery](GPU-Discovery.md) through a dedicated callback.

### Synchronous producer execution and CPU fallback

Dependency Runs also invoke existing synchronous GPU producers on the context's native worker. The host collects each declared rectangular input demand into packed native storage, independently charges its rounded capacity, and supplies `ps_gpu_service_v11` for the callback lifetime. GPU callbacks must submit actual native work. Native invocation views drain before owners retire or CPU retry. Whole producers preserve their full-domain validation and evidence.

Only `BackendUnavailable` with both CPU support and `allows_cpu_fallback` permits retry. A staged failure retires the continuation and restarts the original Q on the CPU worker; it never polls GPU state with CPU services. Cancellation wins. Per-session limits apply to each attempt, and all attempts consume the bounded Run work. Diagnostics include actual backends and rejected attempt timings. Fallback ancestry propagates through child results, shared Flights and retained Whole records. Such results cannot populate dependency result caches, and their stages cannot use or retain checkpoints or pure blocks under native identities. Independent waiter cancellation still cannot abort a producer another waiter needs.

Before a staged GPU attempt, the Run saves bounded record coverage. CPU restart restores that coverage, including previously merged rows, and rebuilds local indexes so abandoned ancestors do not consume the retry's record allowance. The save/restore work is charged without refunding GPU discovery work. Shared immutable records, Flights and cache epochs are unchanged. Whole pixels and their complete structural DAG retain their at-most-once lifetime; their evidence is reimported only if the restarted path actually requests those pixels.

The public `sync_main.cpp` workflow checks separated samples `{0,2}` against the independent `x+1` oracle (1 and 3), real Metal dispatches, missing-device CPU fallback, synchronous Whole and mid-program staged fallback followed by a GPU descendant, actual attempt diagnostics, fresh native retry without stale hits, and exact Whole source evidence. Two 20000-byte payloads each require a 32768-byte native allocation; together they require 65536 live bytes: 65535 fails, 65536 succeeds. Rejection/CPU retry and a fresh native retry also fit that same budget after earlier output owners retire. Its nonnative fallback checks run before returning 77 on hosts without Metal. `test_execution_demand` also checks shared fallback with producer-waiter cancellation and zero cache retention. Additional native cases verify CPU constant fallback within 16 metadata entries after an Elementwise ancestor and 12 after a Whole ancestor, restoration of a prior output's rows, and Whole reuse with complete evidence in ordinary and frozen execution. A portable record test preserves an independently shared ancestor/child DAG across local rollback and reimport.

## Algorithms and wire format

Each tile has at most 64 logical samples. Callers can specify positive rank-sized tile extents with product at most 64, or use the deterministic default geometry that fills the rightmost axes first. Only occupied tiles enter the directory. Global shape products need not fit uint64: lookup uses per-axis tile coordinates, never a flattened global index. `ValueFragments` rejects structural image facets that require `PlanarImage`; the atlas accepts generic Values and applies any accepted ColorArray complete-tuple rule without creating planar image execution support.

The hash is FNV-1a over the rank coordinate words, low byte first, with offset basis 14695981039346656037 and prime 1099511628211. Local row-major coordinates select a mask bit. A present sample's payload offset is the tile byte offset plus its preceding set-bit count times dtype width. Payload is ordered by tile key and increasing valid mask bit. No hole occupies a payload sample. An empty port uses one inert payload byte and a two-slot empty directory; neither authorizes samples.

`prepare` reports exact logical allocation sizes for payload and directory before materialization. A GPU host must round each separately to actual device capacity before admission. `maximum_boxes` bounds occupied tiles, while `maximum_work` bounds metadata scanning, sample discovery, hashing/probing and packing. Entry cancellation and metadata precharges precede cardinality/equality scans. Successful plans expose `preparation_work()` and `materialization_work()` so an enclosing Run can account these operations. Cancellation after either allocation, including an empty input, retires all newly owned buffers and publishes no atlas.

## Limitations and failure handling

### Verification evidence

The installed consumer uses the public preparation/materialization/address APIs in [test_fragment_atlas.cpp](../../tests/unit/test_fragment_atlas.cpp). It checks four dtypes (UInt8, Int64, Float32 and Float64), exact wire words, holes including bit 63, changed input bits, negative strides, a rank-8 domain whose dense size exceeds uint64, precise allocation/work frontiers, source-owner retirement and cancellation during allocation. The native GPU test passes the same SDK helper to the real Metal service and compares returned flags and bits with an independent coordinate dictionary. The native run performs seven dispatches over ranks 1/3/8 and widths 1/4/8, including 65 separate fragments with four bindings, directory capacity rounding above 16 KiB, a one-byte-below native allocation failure, global out-of-domain queries and internal holes. Exit 77 means native Metal is unavailable and is a skip, not native success. The successful transport result does not establish bounded GPU discovery completion.

Atlas preparation and packing preserve only samples in the authorized generic Value coverage. Structural image facets are rejected by `ValueFragments`; use the separate `PlanarImage` contract for image storage. Tile count and work limits reject oversized plans, cancellation retires newly allocated owners, and missing directory entries return `NotFound`. Device hosts must admit rounded payload and directory capacities separately; atlas lookup does not trigger reads or provide a CPU fallback.
