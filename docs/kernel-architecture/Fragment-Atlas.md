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

C++ staged GPU Runs now integrate this transport as described below. Bounded
GPU discovery remains G4 work; the C staged bridge shares this transport. Legacy synchronous
GPU producers within dependency templates also remain unsupported. Raw
Float64/Int64 transport does not assert native floating-point arithmetic support.

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
bounded GPU discovery completion.


## Staged native execution

A GPU `DependencySession::poll` requires complete `DependencyGpuServices` from
its host. The Run uses the existing GPU worker, waiting admission, native device
and synchronous `Invocation`; start/source callbacks retain the CPU worker.
There is no second executor or shader page-fault mechanism. State-cache hits and
control/constant-only paths may complete with zero new dispatches. Selected
backend identifies the implementation/numeric contract; actual native work is
reported separately in dispatch/submission/device-time counters and poll timings.

`phase.atlas(port)` lazily prepares and materializes only that stage's validated
port coverage. It charges preparation and packing work before the host allocates
and reuses the same atlas on repeated calls within a poll. Missing stage ports
and CPU use fail. Native buffer acquisition and execution are fenced and sticky;
ignored failures cannot publish success. The host keeps all acquired native
views until the synchronous callback drains. Caller and auxiliary set
cancellation are combined for ordinary, streamed and frozen dependency Runs;
cancellation wins over an earlier native service error.

Each atlas has a separate nonblocking MemoryBudget reservation for the exact
native payload and directory capacities. Stage output bytes round each requested
rectangle separately; declared workspace bounds count actual native capacity.
Sealing releases only unused reservation bytes. Live atlases, state, scratch and
outputs remain charged until their last owner retires. A stage that cannot fit
with its retained owners fails finitely. Atlas leases never authorize additional
input samples, batching or changes to per-output certificates.

The public [G4 GPU workflow](../../examples/g4_gpu_workflow/main.cpp) reads one
Int64 control per observation on the host, declares 65 isolated Float32 samples,
and performs a real Metal atlas sum with three bindings. Independent arithmetic
requires results 2145, 4290 and 2145. Two observations dispatch; the third reuses
a pure block with identical incoming control/state and currently supplied data,
while preserving its own control `{2}` evidence. The shader explicitly checks
missing samples before numerical publication. Additional runs verify the exact
33079/33080-byte failed/successful stage reservation frontier and owner reuse,
plus ordinary streaming cancellation after a native service error.

```sh
cmake --build build/issue257-static --target photospider_g4_gpu_workflow test_dependency_gpu -j 8
ctest --test-dir build/issue257-static -R '^(test_dependency_gpu|photospider_g4_gpu_workflow)$' --output-on-failure
build/issue257-static/photospider_g4_gpu_workflow
```

`test_dependency_gpu` uses explicitly nonnative mocks for protocol-only service
absence, CPU misuse, host exceptions, ignored errors, work-before-allocation and
atlas reuse/retirement. It does not count as native evidence. Installed static
and shared consumers run that test and compile the same public workflow. The
workflow returns 77 on hosts without native Metal, after verifying its CPU oracle.


## C staged GPU bridge

Both `ps_dependency_services_v8` and `ps_dependency_block_services_v8` expose
`atlas`, `gpu_buffer` and `gpu_execute`, with Boolean 1 success / 0 failure.
This return convention differs from `ps_gpu_service_v8` result codes. A
`ps_dependency_atlas_v8` reports full-domain shape/tile geometry, slot count,
valid sample bytes, physical binding spans and immutable payload/directory
view tokens. It exposes no new source reads. Repeated same-port lookup returns
the same tokens within a poll. CPU use fails with InvalidArgument.

The adapter maps native view tokens to invocation-monotonic C handles. Only the
current poll's map can translate dispatch bindings; previous-poll, forged or
other-kind handles cannot accidentally refer to a newly reused native token.
Output publication revokes writable access even for an already acquired token.
The C adapter bounds command count to 32, bindings to 31 per command and views
to 1024 per poll, and charges command/binding metadata before copying. Native
validation still enforces source, constants, access and grid bounds. Every
service error is sticky, including ignored errors inside pure compute. Atlas
and native view owners survive synchronous drain, then retire with their normal
Phase/Invocation leases.

The public C11 plugin `examples/g4_gpu_workflow/c_plugin.c` and loader
`c_main.cpp` run a real two-observation workflow. Each host control read selects
65 isolated Float32 samples. The expected sum is 2145 for both observations;
there is one actual Metal dispatch and one block hit. The second observation
retains current control `{1}` and excludes obsolete control `{0}`. Retained
native output/state capacity is 16 bytes. Negative runs cover stale/forged
handles, malformed atlas records, invalid view/command/binding arguments,
immutable input promotion, writes after publication and actual shader misses.
A missing sample returns failure before numerical output publication. Each
failed case is followed by a real uncached retry in the same tight-budget context,
so an old cached output cannot hide retained failure-path allocations.

```sh
cmake --build build/issue257-static --target photospider_g4_c_gpu_workflow -j 8
build/issue257-static/photospider_g4_c_gpu_workflow
```

The standalone example CMake project and static/shared installed consumers also
build this C11 module and its public loader. An optional loader argument selects
a module path. Exit 77 means native unavailable after CPU checks, never native
success. This bridge does not itself implement a discovery request table.
