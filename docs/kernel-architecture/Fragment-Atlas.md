# Sparse fragment atlas transport

`FragmentAtlasPlan` prepares transport metadata from an exact `ValueFragments`
coverage, then materializes that plan through a caller-supplied allocator. It
supports rank 1..8 and UInt8/Int64/Float32/Float64 without converting sample bits.
The plan retains domain/coverage and tile masks, with no source Value/storage
owners. Materialization reads the current matching input, including negative
strides and separate physical fragments. It never fills a bounding-box gap.

Each tile has at most 64 logical samples. Callers can specify positive rank-sized
tile extents with product at most 64, or use the deterministic default geometry
that fills the rightmost axes first. Only occupied tiles enter the directory.
Global shape products need not fit uint64: lookup uses per-axis global tile
coordinates, never a flattened global index. Image channel coordinates remain
part of the original domain; this transport does not change image-v2 closure or
supply validation rules.

The directory has a power-of-two slot count, at least two and at least twice the
occupied tile count. Slots use open addressing with linear probing. Each slot is
80 little-endian bytes:

| Bytes | Meaning |
| --- | --- |
| 0..63 | Eight uint64 global tile coordinates; unused axes zero |
| 64..71 | uint64 valid-sample mask; zero means an empty slot |
| 72..79 | uint64 start byte offset in the packed payload |

The hash is FNV-1a over the rank coordinate words, low byte first, with offset
basis 14695981039346656037 and prime 1099511628211. Local row-major coordinates
select a mask bit. A present sample's payload offset is the tile byte offset plus
its preceding set-bit count times dtype width. Payload is ordered by tile key and
increasing valid mask bit. No hole occupies a payload sample. An empty port uses
one inert payload byte and a two-slot empty directory; neither authorizes samples.

`prepare` reports exact logical allocation sizes for payload and directory before
materialization. A GPU host must round each separately to actual device capacity
before admission. `maximum_boxes` bounds occupied tiles, while `maximum_work`
bounds metadata scanning, sample discovery, hashing/probing and packing. Entry
cancellation and metadata precharges precede cardinality/equality scans. Successful
plans expose `preparation_work()` and `materialization_work()` so an enclosing Run
can account these operations. Cancellation after either allocation, including an
empty input, retires all newly owned buffers and publishes no atlas.

The public `FragmentAtlas::address` and the C-compatible SDK macro
`PS_FRAGMENT_ATLAS_MSL_V8` implement matching lookup. The latter defines
`ps_atlas_address` for a Metal shader. The host supplies actual binding spans,
slot count, shape, tile extents and payload byte count. Only payload and directory
buffers are needed regardless of fragment count. Boundary mapping must occur in
global coordinates before lookup. An absent mask bit/slot is an explicit missing
sample, never a zero value or per-fragment clamp.

This is implemented transport and shader lookup. Integration with staged GPU
scheduling and bounded GPU discovery is still G4 work; existing dependency Runs
continue to reject native stages until that path is connected. Raw Float64/Int64
transport does not assert native floating-point arithmetic support.

## Public and native verification

The installed consumer uses the public preparation/materialization/address APIs
in [test_fragment_atlas.cpp](../../tests/unit/test_fragment_atlas.cpp). It checks
all four dtype widths, exact wire words, holes including bit 63, changed input
bits, negative strides, a rank-8 domain whose dense size exceeds uint64, precise
allocation/work frontiers, source-owner retirement and cancellation during
allocation. Run the package test or its installed executable:

```sh
ctest --test-dir build/issue257-static -R '^test_installed_consumer$' --output-on-failure
build/issue257-static/consumer-build/photospider_fragment_atlas_consumer
```

The noninstalled native test passes the same SDK helper to the real Metal service
and compares GPU-returned flags/bits with an independent coordinate dictionary:

```sh
cmake --build build/issue257-static --target test_fragment_atlas test_fragment_atlas_gpu -j 8
ctest --test-dir build/issue257-static -R '^(test_fragment_atlas|test_fragment_atlas_gpu)$' --output-on-failure
build/issue257-static/test_fragment_atlas_gpu
```

The native run performs seven dispatches over ranks 1/3/8 and widths 1/4/8,
including 65 separate fragments with four bindings, directory capacity rounding
above 16 KiB, a one-byte-below native allocation failure, global out-of-domain
queries and internal holes. Exit 77 means native Metal is unavailable and is a
skip, not native success. The successful transport result does not establish
staged GPU/discovery workflow completion.
