# Pack sparse fragments into an atlas

## Scope and ownership

The plan retains descriptor-domain metadata, exact sample coverage and occupied tile coordinates. Materialized payload and directory Values own packed bytes through the supplied allocator; a Result-backed atlas retains no source Result or read window. A native host keeps each device view alive until the synchronous invocation drains. Atlas support for all scalar widths is transport support only; it does not imply GPU arithmetic support for a dtype.

The atlas packs generic tensor sample bits into one payload and a sparse tile directory. Callers can prepare it from `ValueFragments` or from the exact coverage carried by a `ResultTensorInput` capability. The Result overload reads only that capability's authorized samples; it does not convert a Result to Value semantics or broaden input authorization. The older `ValueFragments` overload remains available to its existing callers: it rejects structural image metadata that requires `PlanarImage`, while accepted ColorArray tuples preserve their full-tuple rule. Atlas packing alone does not implement image operations or grant new reads. Generic directory planning and packing are implemented in the data layer. The `ResultTensorInput` overloads are implemented in the plugin layer beside the Result input capability and adapt its coverage and owning windows to the generic packer, using the same packing work policy.

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

`FragmentAtlasPlan` prepares transport metadata from exact coverage and materializes it through a caller-supplied allocator. It supports rank 1..8 and every built-in `ElementType` (UInt8, Int64, Float64, Float32, Int8, UInt16 and Int16) without converting sample bits. The plan retains domain, coverage and tile masks, but no source data owners. The `ResultTensorInput` overload acquires owning windows for the Need-authorized samples and copies them through `row_run()`. It supports signed strides, zero-stride broadcasts, fragmented windows and batch-prefixed spatial tensors without filling holes or bounding boxes. Once packed, the atlas remains valid independently of the input Result, poll and execution context. When the capability grants no payload read, materialization still routes through `input.read`, so the attempt latches `UnauthorizedRead` exactly as a direct read would.

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

A `ResultTensorInput` atlas packs only samples authorized by that tensor Need. Preparation retains descriptor, coverage and occupied tile coordinates; materialization copies sample bytes into Root-accounted payload and directory buffers. The resulting atlas owns those buffers but does not retain the source Result or read windows. The atlas does not grant additional reads, and support for transporting a scalar type does not imply GPU arithmetic support for that type.

Native callbacks use the configured synchronous invocation. The host keeps native views alive until submitted dispatches drain, then releases poll-scoped resources. Missing samples remain holes in the directory. An absent directory entry returns `NotFound`; a CPU-phase call, absent Need or Descriptor-only Need cannot widen the grant. Cancellation, allocation failure and sticky service errors prevent publication and retire newly owned buffers.

The C Result bridge uses the same Need-scoped atlas. Atlas buffer tokens and dispatch services are borrowed for the active poll; releasing either member of the token pair retires the pair, and a later acquisition returns new handles. The host freezes transferred output scratch on publication, so a previously writable token cannot modify published bytes. Each Result output still records its own current association and dependency evidence.

Synchronous GPU attempts may retry on CPU only when the operation permits CPU fallback and the failure is an eligible `BackendUnavailable`. The GPU invocation drains before its views retire or the CPU attempt starts. Cancellation, stale state, protocol errors, published output and other non-retryable failures do not become CPU retries. Result cache retention and active producer sharing remain separate from this fallback decision.

Current integration sources exercise the transport and failure boundaries:

- [`test_gpu_fragment_execution.cpp`](../../tests/integration/gpu/test_gpu_fragment_execution.cpp) compares real native lookup and output against an independent coordinate oracle. It covers rank, width, sparse holes, fragment packing, grant enforcement and Root admission.
- [`test_gpu_c_abi_execution.cpp`](../../tests/integration/gpu/test_gpu_c_abi_execution.cpp) loads [`gpu_result_c_plugin.c`](../../tests/fixtures/gpu_result_c_plugin.c) and checks C atlas handles, stale tokens, publication revocation, malformed requests, errors and native dispatch.
- [`test_fragment_atlas_gpu.cpp`](../../tests/unit/test_fragment_atlas_gpu.cpp) exercises the lower-level atlas SDK with native buffer capacity and readback checks.
- [`test_gpu_sync_fallback.cpp`](../../tests/integration/gpu/test_gpu_sync_fallback.cpp) checks CPU retry, cancellation and retained producer behavior.

These native tests return the configured skip code when the required device is unavailable. Protocol mocks and CPU fallback do not establish native execution.
## Algorithms and wire format

Each tile has at most 64 logical samples. Callers can specify positive rank-sized tile extents with product at most 64, or use the deterministic default geometry that fills the rightmost axes first. Only occupied tiles enter the directory. Global shape products need not fit uint64: lookup uses per-axis tile coordinates, never a flattened global index. `ValueFragments` rejects structural image facets that require `PlanarImage`; the atlas accepts generic Values and applies any accepted ColorArray complete-tuple rule without creating planar image execution support.

The hash is FNV-1a over the rank coordinate words, low byte first, with offset basis 14695981039346656037 and prime 1099511628211. Local row-major coordinates select a mask bit. A present sample's payload offset is the tile byte offset plus its preceding set-bit count times dtype width. Payload is ordered by tile key and increasing valid mask bit. No hole occupies a payload sample. An empty port uses one inert payload byte and a two-slot empty directory; neither authorizes samples.

`prepare` reports exact logical allocation sizes for payload and directory before materialization. A GPU host must round each separately to actual device capacity before admission. `maximum_boxes` bounds occupied tiles, while `maximum_work` bounds metadata scanning, sample discovery, hashing/probing and packing. Entry cancellation and metadata precharges precede cardinality/equality scans. `preparation_work()` reports generic geometry-planning work and `materialization_work()` reports generic packing work. The `ResultTensorInput` overload precharges its work through `FootprintLimits::consume_work`; Result materialization additionally precharges window acquisition and read-bound work that `materialization_work()` does not include. The `ValueFragments` overloads preserve the existing caller-charged convention. Cancellation after either allocation, including an empty input, retires all newly owned buffers and publishes no atlas.

## Limits and failure handling

The generic atlas unit suite is registered as `test_fragment_atlas` from [`tests/unit/test_fragment_atlas.cpp`](../../tests/unit/test_fragment_atlas.cpp). The native SDK suite uses [`tests/unit/test_fragment_atlas_gpu.cpp`](../../tests/unit/test_fragment_atlas_gpu.cpp); the Result callback integration path uses [`tests/integration/gpu/test_gpu_fragment_execution.cpp`](../../tests/integration/gpu/test_gpu_fragment_execution.cpp). These sources cover wire layout, coordinate lookup, sparse holes, signed and zero strides, rank and dtype transport, buffer admission, cancellation and native readback. Native tests may skip when the required Metal device is unavailable.

The `ValueFragments` overload preserves its accepted Value coverage; structural image facets that require `PlanarImage` are rejected. The `ResultTensorInput` overload reads only samples authorized by its tensor Need. Both packers preserve holes, return `NotFound` for missing directory samples, and fail plans that exceed tile or work limits. Cancellation retires newly allocated owners. Device hosts must admit rounded payload and directory capacities separately; atlas lookup does not trigger additional reads or provide a CPU fallback.
