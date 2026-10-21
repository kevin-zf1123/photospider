# Pack sparse fragments into an atlas

[Chinese reader version](zh/Fragment-Atlas.zh.md).

## Scope and ownership

`FragmentAtlasPlan` is a data-layer transport utility for a `Tensor`. Preparation records its descriptor, exact coverage and occupied tile coordinates. Materialization copies the covered sample bits into independently owned payload and directory buffers. The plan retains no source storage; the materialized atlas remains valid after the input Tensor is released.

Atlas transport supports all built-in scalar widths. This does not establish GPU arithmetic support. The fixed executor passes the views declared in its physical Plan; atlas packing is an explicit data operation, outside the fixed kernel invocation service table.

## Public interface

```cpp
struct FragmentAtlas final {
  TensorDescriptor descriptor;
  std::vector<std::uint64_t> tile_shape;
  std::uint64_t slot_count = 0, payload_bytes = 0;
  TensorSpan payload, directory;
  Result<std::uint64_t> address(
      const std::vector<std::uint64_t>& coordinate) const;
};

class FragmentAtlasPlan final {
 public:
  static Result<FragmentAtlasPlan> prepare(
      const Tensor& input, std::vector<std::uint64_t> tile_shape = {},
      const FootprintLimits& limits = {});
  Result<FragmentAtlas> materialize(
      const Tensor& input, const BufferAllocator& allocator,
      const FootprintLimits& limits = {}) const;
};
```

These declarations omit size/work accessors and private state; the complete API is in [`fragment_atlas.hpp`](../../include/photospider/data/fragment_atlas.hpp). A materialization input must match the prepared dtype, shape and coverage. Its current sample values are read at materialization, so one plan may pack different values with the same static geometry.

Packing preserves UInt8, Int64, Float64, Float32, Int8, UInt16 and Int16 bits. Tensor reads support fragmented, signed-stride and broadcast storage. Missing samples remain holes. Lookup of a hole returns `NotFound`; an out-of-domain coordinate returns `InvalidArgument`.

## Geometry and wire format

Rank is 1..8. Each transport tile contains at most 64 logical samples. Explicit tile extents must be positive, match the rank and have a product at most 64. Empty geometry selects a deterministic layout by filling the rightmost axes first. Atlas transport tiles are independent of physical Plan tiles.

Only occupied tiles enter the directory. Global shape products need not fit uint64: lookup uses per-axis coordinates. The directory has a power-of-two slot count, at least two and at least twice the occupied tile count. Slots use open addressing with linear probing. Each slot is 80 little-endian bytes:

| Bytes | Meaning |
| --- | --- |
| 0..63 | Eight uint64 global tile coordinates; unused axes zero |
| 64..71 | uint64 valid-sample mask; zero means an empty slot |
| 72..79 | uint64 start byte offset in the packed payload |

The hash is FNV-1a over the rank coordinate words, low byte first, with offset basis 14695981039346656037 and prime 1099511628211. Local row-major coordinates select a mask bit. A present sample's payload offset is the tile byte offset plus its preceding set-bit count times dtype width. Payload is ordered by tile key and increasing valid mask bit. Holes occupy no payload samples.

An empty Tensor uses one inert payload byte and a two-slot empty directory. It contains no readable samples. Boundary mapping belongs to the sampling algorithm and must occur in global coordinates before lookup.

The C-compatible SDK macro `PS_FRAGMENT_ATLAS_MSL_V11` defines `ps_atlas_address` for a Metal shader with the same directory format and hash. The shader helper accepts only 1-, 4- and 8-byte samples; it rejects the 2-byte width used by UInt16 and Int16. CPU packing and `FragmentAtlas::address` support those two types. Its host must supply valid buffer spans, shape, tile extents, slot count and payload byte count. The fixed executor links GPU placements through native kernel preparation and asynchronous completion. Atlas packing remains explicit: the operation declares the packed buffers and their fixed read views. `test_fragment_atlas_gpu` executes the Metal address helper against sparse atlas payloads at ranks 1, 3 and 8; public physical Plan integration is separately exercised by `test_fixed_native`.

## Resource bounds and failures

`payload_allocation_bytes()` and `directory_allocation_bytes()` report exact logical allocation sizes before packing. A device host must admit each rounded physical capacity separately. Payload and directory retain the allocator's storage owners.

`maximum_boxes` bounds occupied tiles. `maximum_work` bounds metadata scanning, sample discovery, hashing, probing and packing. `preparation_work()` and `materialization_work()` report the corresponding work estimates for caller accounting. The Tensor entry points enforce their local limits and cancellation token but do not invoke `FootprintLimits::consume_work`.

Invalid inputs, geometry or materialization mismatches fail without publishing an atlas. Work/capacity exhaustion and cancellation release newly allocated owners, including cancellation after either allocation. Size/work accessors on an invalid plan throw `std::logic_error`; `valid()` returns false.

[`test_fragment_atlas.cpp`](../../tests/unit/kernel/test_fragment_atlas.cpp) checks the data utility's bytes, sparse lookup, geometry, strides, ownership, bounds and cancellation. Native SDK tests are a separate validation surface; a successful CPU test does not establish device execution.
