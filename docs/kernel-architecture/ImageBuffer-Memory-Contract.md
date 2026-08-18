# ImageBuffer Memory Contract

`ImageBuffer` is a public operation and Host value contract. Operations,
plugins, adapters, cache code, Host implementations, and debugging tools may
depend on the fields and invariants in this document. Scheduler contracts do
not inspect image payloads.

## Structure

| Field | Meaning |
| --- | --- |
| `width` | Image width in pixels. |
| `height` | Image height in pixels. |
| `channels` | Number of channels per pixel. |
| `type` | Channel data type. |
| `device` | Where the authoritative data lives. |
| `step` | Row stride in bytes. |
| `data` | CPU-accessible data owner or view. |
| `context` | Backend-specific resource owner or handle. |

The public tile contract uses `InputTileView` and `OutputTileView`. Both carry a
borrowed `const ImageBuffer*` plus a backend-neutral `PixelRect`. The input view
is read-only. The output view permits adapters to expose writable pixels from
the retained payload, but the callback cannot replace descriptor dimensions,
device identity, payload ownership, or backend context. Public
`TiledOperation` callbacks receive the output as `const OutputTileView&` and
receive inputs as `OperationTileInputView` values.

The private compute layer separately uses `InputTile`, `OutputTile`, and
`TileTask`. Those backend-only values also carry zero-based storage-relative
`PixelRect`; private `OutputTile` borrows an immutable
`DenseImageOutputPlan` and an active `HostOutputWriteGrant` while bridging
Host-owned output storage to an adapter. The grant Region is instead expressed
in the plan's signed logical data-window domain. An OpenCV adapter adds the
plan origin with checked arithmetic before comparing ROI endpoints to that
grant, but retains the zero-based ROI for byte offsets and matrix construction.
It does not retain or return `cv::Rect` through these values. They do not cross
the public operation or Host contract.

## CPU Buffer Contract

For CPU buffers owned by the kernel:

- `device == Device::CPU`.
- `data != nullptr` when image dimensions are non-zero.
- `step >= width * channels * bytes_per_channel(type)`.
- `step` may include padding.
- The base pointer must be 64-byte aligned.
- Every row start must be 64-byte aligned.

Every row start alignment means:

```text
address(row y) = data + y * step
address(row y) % 64 == 0
```

Therefore, kernel-owned allocations must pad `step` so that row starts stay
aligned even when packed row size is not a multiple of 64.

These alignment and mutability rules apply specifically to CPU buffers
allocated and owned by the kernel. `ImageBuffer::data` is a shared lifetime
handle, not a universal writable-memory promise. Producers may return a
read-only CPU snapshot and must document that boundary.

The installed IPC Host's `compute_and_get_image` result is such a snapshot. It
validates a same-user private artifact while its delivery lease protects
result-to-open, then maps the exact tight-row file with
`PROT_READ|MAP_PRIVATE`. The mapping base has the platform's page alignment,
but `step` is the packed row width, so later row starts are not promised to be
64-byte aligned. Copies of the descriptor share the mapping; the final
reference unmaps and closes its retained descriptor exactly once. Writing
through this mapping is outside the contract and may fault. A consumer that
needs writable storage or kernel-owned per-row alignment must allocate an
appropriate CPU buffer and copy rows using `step`.

## ARM Mac Alignment

64-byte row alignment is the current portable minimum. The contract makes no
128-byte guarantee on ARM Mac or any other platform.

## Stride-Aware Access

Consumers must use `step` when walking rows. They must not assume the buffer is
tightly packed.

Correct row access pattern:

```cpp
auto* row = static_cast<unsigned char*>(buffer.data.get()) + y * buffer.step;
```

Incorrect assumption:

```cpp
auto* row = base + y * width * channels * bytes_per_channel;
```

OpenCV adapters must preserve stride by constructing `cv::Mat` with the
provided `step`.

## Kernel CPU Buffer Primitives

The standard-library-only operation runtime owns the current minimum CPU buffer
primitives:

- `validate_image_buffer()` validates declared enums, canonical empty state,
  positive nonempty dimensions, shared-owner consistency, CPU payload
  requirements, packed-row stride, and representable descriptor byte
  arithmetic. Opaque backend allocation capacity remains provider-owned.
- `image_buffer_row_bytes()` computes active packed-row bytes without padding,
  while `image_buffer_row_data()` returns a read-only CPU row through `step`.
- `fill_image_buffer_region()` fills only the active bytes of an
  `OutputTileView`. Pixels outside the ROI and row padding are unchanged.
- `copy_image_buffer_region()` copies equal-shaped, equal-format
  `InputTileView`/`OutputTileView` regions row by row. It snapshots all source
  active bytes before the first destination write when the payloads may alias,
  so overlapping views have value-copy semantics. Proven-independent payloads
  copy directly after validation; validation/allocation failures leave the
  destination unchanged.

The tile views are borrowed for each call. The primitives do not retain
descriptors, add synchronization, infer backend mappings, or turn a read-only
producer snapshot into writable memory. Row/pixel access requires a nonempty
owned CPU payload. Because `shared_ptr` does not expose allocation capacity, the
producer must ensure that storage covers every declared active row. Valid
context-only or non-CPU descriptors are rejected without dereferencing them.
Copy and fill validate the complete descriptor and ROI before mutation, and
never treat padding as pixels.

Current tiled `image_mixing` crop/pad normalization composes aligned allocation,
zero fill, and region copy. Exact-shape inputs remain pass-through descriptors.
Resize and channel conversion still use OpenCV only at the actual algorithm
call and return an `ImageBuffer` retaining the result. The compute metrics
recorder no longer creates or reshapes `cv::Mat`: when timing statistics are
enabled, it walks active CPU scalar values through `step`, excludes padding,
and records range/non-finite diagnostics. An all-NaN active payload retains the
previous positive/negative infinity empty-range sentinels. Opaque non-CPU
resources retain provider-supplied diagnostics because only their device
adapter may map them.

## V-3 Value-Backed CPU Ownership Bridge

The dependency-neutral operation runtime also implements the installed
`BufferHandle`, `ValueBuilder`, `Value`, `DenseTensorView`, and `ImageView` CPU
subset. `BufferHandle` is a checked nonempty byte range over one process-local
allocation identity. Consumers acquire retaining `ReadLease` objects;
`ValueBuilder` alone can acquire one move-only `WriteLease`, refuses to seal
while that lease is live, and revokes further writes when it publishes the
immutable bytes with a fresh process-local `ValueRevisionId`.

Concrete shape and element facts remain separate from an optional explicit
Image Facet and a `StridedLayout`. Producer layouts require a zero byte offset
and positive exact-envelope strides. Before allocation or a WriteLease, the
builder orders all non-singleton axes by increasing byte stride. A checked
covered span starts at the element byte width; each next stride must be at
least that span before its axis contribution extends the span. This
rank-general induction proves that writable coordinate slabs do not overlap
without enumerating elements. Singleton axes add no alternative address and
zero extents remain invalid descriptors. Contiguous, padded, transposed, and
other axis-permuted layouts pass; single-axis and cross-axis byte collisions or
unrepresentable span arithmetic fail before authority escapes. Immutable
Values constructed over a sealed handle may instead use a bounded byte offset
plus positive, zero, or negative signed byte strides. Checked views retain both
the complete Value and a read lease, and expose no writable pointer.

The built-in `image_process:invert_dense` operation proves this surface on the
production path:

1. the current monolithic callback validates one nonempty CPU image input;
2. the private runner requires the canonical named `image` Value and never
   falls back to formal compatibility storage;
3. pure inference receives only a deep-owned effective parameter snapshot and
   logical DenseTensor/Image descriptors, with no Node output/cache state;
4. execute receives checked ImageViews, follows explicit x/y/channel strides,
   and returns a separately owned sealed unsigned-8 Value; and
5. the runner compares the complete result descriptor and Image Facet with
   inference and publishes that exact result allocation/revision as the
   `NodeOutput` named `image` Value. Current external consumers derive
   use-scoped compatibility snapshots only at their explicit adapters.

Malformed caller input is `GraphErrc::InvalidParameter`; an unsupported or
mismatched operation result is `GraphErrc::ComputeError`; `std::bad_alloc`
propagates unchanged. The runner and compatibility adapter remain source-tree
private. Operation ABI v1 carries complete Value descriptor/facet/layout and
Host-owned grant records. Unconverted internal/Host/codec paths may still use
ImageBuffer; inbound results are normalized before private return and the
formal HP cache stores only sealed named Values.

## DI-2 Host-Owned Output Authorization

DI-2 added no public `ImageBuffer` field and froze one source-private
`DenseImageOutputPlan` before allocation. DI-3 projects that same authority
through exact pure-C operation ABI v1 plan and grant records. The private plan
plan owns the canonical output name, complete DenseTensor/ImageFacet metadata,
positive interleaved Strided layout, exact storage envelope, required
power-of-two alignment, and full image Region. All extent, stride, row-span,
offset, alignment, range, and overflow checks finish before a producer can see
mutable bytes.

`HostOutputBinding` allocates exactly once through `ValueBuilder` and retains
the sole builder `WriteLease`. Producers receive only move-only revocable whole
or tile grants. A tile grant exposes checked active row spans; live grants must
be logically and bytewise disjoint. Invalid overlap/range/alignment, an
exception, cancellation, explicit failed retirement, duplicate retirement, or
destruction while active records the first sticky failure and revokes every
grant. Seal fails while any grant remains active and cannot be retried into
success. After exact successful retirement, seal closes issuance, revokes
write access, and publishes one Ready Value with the planned allocation and a
single fresh revision. A second seal cannot mint another revision.

Tile grant authorization and callback geometry deliberately use two coordinate
representations. `HostOutputWriteGrant::image_region()` is a signed logical
`ImageRect` contained by `ImageFacet::data_window`; `OutputTile::roi` is a
zero-based storage rectangle contained by plan width/height. They describe the
same pixels only after checked origin translation. Grant span offsets and
strides remain allocation-relative, so a negative or nonzero logical origin
does not alter storage view construction.

The current operation ABI v1 tiled adapter emits exact callback-scoped
`OutputGrantSpan` rows over the active Host grant. It never transfers the
builder, owner, allocator, or seal authority and accepts no out-of-plan row.
CPU monolithic/tiled inputs and codec compatibility inputs are normalized at
their explicit adapters. The synchronous CPU-only operation ABI exposes no
opaque non-CPU result owner or native-device handle.

## DI-1 Ordinary DenseImage Coordinate and Interpretation Contract

Issue #129 adds no field to `ImageBuffer`. An ordinary image-faceted Value now
requires a signed nonempty half-open `data_window`, may carry an independent
`display_window`, and may carry bounded stable channel/group, declared
sample-domain, and color records. The data-window spans exactly match the
explicit x/y tensor axes; negative and nonzero logical origins are valid.
`ImageView::channel_data()` remains a zero-based storage-index API, while
`channel_data_at()` checks signed logical coordinates and subtracts the data
window origin. Bounds metadata is inspectable without payload readiness; both
view constructors still require a Ready host-readable payload.

The compatibility projection is deliberate and asymmetric:

- `ImageBuffer -> Value` creates `[0,width) x [0,height)` and no display,
  stable channel/group, sample-domain, or color facts because ImageBuffer
  supplies none;
- `Value -> ImageBuffer` copies active extents and elements but cannot preserve
  signed origin or richer interpretation; converting that snapshot back creates
  the documented zero-origin projection; and
- pure built-in DenseImage descriptor inference copies the complete ImageFacet,
  while a bounded edge that cannot encode present metadata rejects it before
  allocation or callback entry rather than silently deleting fields.

Storage-representable range, quantization, declared sample domain, color, and
observed statistics remain separate. In particular, no ImageBuffer type or
channel name implies normalized range, RGB, alpha, transfer function, or
primaries. Provider-defined OpenEXR Deep windows remain outside this ordinary
DenseImage authority.

## GPU Buffer Contract

For GPU buffers:

- `device` identifies the backend, such as `Device::GPU_METAL`.
- `data` may be null.
- `context` carries the backend resource.
- Adapters define how to upload, download, and interpret backend resources.

The public contract is the `device` plus `context` relationship. The concrete
object stored in `context` is backend-specific.

## Metal Buffers

The Metal buffer adapter is implemented in
`src/lib/adapters/metal/buffer_adapter_metal.{hpp,mm}`, but it is not currently
enabled in the core library build and is not connected to the production
compute path. `CMakeLists.txt` separately builds
`plugins/ops/metal/perlin_noise_metal.mm` as the source-private static target
`photospider_metal_perlin_adapter_internal`. There is no separate dynamic
loader translation unit, loader directory, or operation-plugin registration
for this adapter; it is neither installed nor loaded through the pure-C
operation ABI v1.

Implemented adapter behavior:

- Upload supports `FLOAT32` buffers with 1 or 4 channels.
- Uploaded buffers use `Device::GPU_METAL`.
- `context` owns a Metal texture holder.
- Download returns a new CPU `ImageBuffer`.

Plugin, scheduler, and core compute code must not treat the Metal buffer adapter
as a production runtime boundary. The source-private Metal Perlin adapter does
not use that buffer adapter or retain an `ImageBuffer::context` payload. When a
maintained internal test invokes it after reserved start, it borrows the
process executor's command queue, invocation-scoped allocator, and validated
pipeline cache, then encodes compute plus an explicit texture-to-shared-buffer
blit, installs a native completion handler, commits without waiting, and
source-privately returns a pending host-visible Value. `TaskSubmissionPlan`
releases dependants only after that canonical Value becomes Ready; it creates
no ImageBuffer peer. The production provider seed in
`src/lib/providers/configured_operation_providers.cpp` does not register this
adapter as a Graph operation.
The provider owns no independent native lifecycle, calls neither
`waitUntilCompleted` nor `getBytes`, and cannot make `ImageBuffer::context` a
portable memory contract.

## Boundaries and Rationale

`ImageBuffer` is the current two-dimensional compatibility payload for legacy
Host, codec, and built-in paths; it is not the operation DSO contract. Pure-C
operation ABI v1 carries `ValueDescriptor`, optional ImageFacet, Layout,
Region, and Host-owned output-plan/publication records. `ImageBuffer` channel
count is not structurally limited to four, and `FLOAT64` is a declared scalar
type, but those facts do not promise end-to-end support by every loader,
operation, cache, or adapter.

This payload is not the generic graph value model. Operation results keep
generic non-image Values in `NodeOutput::named_values` and keep parameter
results in the separate `data` map; neither category nor an opaque backend
`context` turns `ImageBuffer` into an arbitrary payload carrier. Adding a new
Value kind, representation, descriptor, handle, or Region requires a separate
versioned design.

Current limitations are explicit:

- built-in operations may implement only selected 1/3/4-channel conversions or
  assume RGBA roles;
- some operation and image-loading paths compute in float32;
- FP4 cannot be represented because scalar size and row addressing assume an
  integral number of bytes per channel element;
- rank, N-dimensional shape/strides, quantization, named channel roles, Deep
  Image samples, and vector objects are not represented;
- `context` cannot substitute for descriptor facts needed by planning, cache
  keys, ROI, or synchronization.

Therefore `ImageBuffer` alone is not advertised as a complete framework
contract for 8/16-channel images or FP64. V-12 verifies that image-faceted
generic Values and the CPU Value/ImageBuffer bridge preserve active logical
elements for 1/3/4/8/16 channels and FP32/FP64, including padded source
layouts; it does not widen image-only operations, selected-precision codecs,
or Host surfaces. FP4, latent Tensor, Deep Image, and vector-scene values are
not represented by `ImageBuffer`. The general `Value`, descriptor, handle, and
region target is documented in the exact
[general data and regions target](../roadmap/Kernel-Evolution.md#general-data-and-regions).

### Readiness, delivery, and persistence are separate

For the installed Value bridge, `ReadyFence::Ready` means that producer access
has stopped and the payload can proceed to a checked access plan. It does not
mean that the enclosing `ComputeRun` committed Graph state, that a cache file
was written, or that a user-visible output is durable.

Readiness also does not authorize a provider-selected output shape. Issue #130
requires the exact staged output to match a Host-frozen authority: the
canonical image keeps its descriptor, ImageFacet, Strided layout, identity,
and trusted-extent checks; each declared generic Value keeps its exact name,
revision/producer identity, supported representation/layout, and every indexed
nonempty `StorageBinding`, without an image-facet requirement. Provider output
cannot widen either named category or move a generic Value into parameter data.
A supervised native producer may return exact named Values while Pending. The
Run then installs non-inline, Run-scoped continuations that keep owners alive
without occupying a worker or releasing dependants, chaining multiple Pending
names deterministically. Only after the same exact publications are all Ready
may formal commit proceed; inline/sequential and direct formal dirty paths
reject Pending synchronously. Failed, ProducerCancelled, cancelled, stale, or
replaced state closes without Graph/RT mutation; callback registration and
retained-context drainage prevent duplicate terminal publication or an
abandoned callback owner.

The current IPC image-result path materializes a tight-row CPU artifact in the
private daemon `OutputStore`, calls file `fsync`, atomically renames without
replacement, revalidates filesystem identity, and returns metadata protected by
a process-scoped delivery lease. The store does not synchronize the containing
directory or persist its record/index, and lease/TTL cleanup can unlink the
artifact. The resulting `ImageBuffer` mapping is valid owned delivery state,
not a crash-durable output receipt.

The legacy `io/save` operation independently calls `cv::imwrite` during
provider execution. Its successful return reports that codec call only; the
side effect can precede Run commit and has no OutputStore transaction. These
current limits and the separate target output authority are fixed by
[ADR 0009](../adr/0009-compute-io-durability-and-completion-semantics.md).

### Implemented V-3/V-4/V-6/V-8/V-9/V-12/V-13/V-14/V-15 relationship and remaining target

[ADR 0008](../adr/0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md)
accepts the complete replacement:

```text
ImageBuffer -> Value + ImageFacet + ImageView
DataType    -> ElementSemantics + StorageEncoding + QuantizationSchema
Device      -> DeviceBackend + DeviceId + MemoryDomain
PixelRect   -> RegionSet atom ImageRect
```

The target separates logical `DataDescriptor` from physical
`StorageBinding`, and accesses memory only through checked `BufferHandle`
ranges and leases. V-2 introduced the CPU DenseTensor descriptor, ImageFacet,
positive producer `StridedLayout`, immutable byte ownership, and checked
ImageView used by the bounded operation bridge. V-3 adds public checked
BufferHandle ranges, retaining read leases, exclusive builder write leases,
process-local allocation/revision identities, byte offsets, bounded signed and
zero-stride immutable views, and allocation identity authority in formal HP
cache entries.

V-4 adds the installed dependency-neutral Region MVP. Exact built-in ImageRect
can be projected directly to a logical-coordinate `PixelRect` at legacy edges.
Compute/dirty storage projections are a separate checked operation that clips
to explicit `ImageBounds` and subtracts its origin; the reverse adds that
origin after storage containment. TensorSlice, custom domains, multi-atom
clauses, uncertainty, and overflow are rejected; Whole is accepted only by the
explicit finite-bounds storage projection.
The Region-aware core dense operation copies unselected bytes and changes only
selected logical coordinates through checked strides. ImageBuffer structure,
device field, tiled writes, codecs, and Host/IPC v2 rectangles remain role-
specific compatibility contracts until their owning later slices migrate
them; DI-3 has already migrated the operation DSO boundary to pure-C ABI v1.
Formal CPU image cache entries carry only the valid
sealed Value; compatibility snapshots are use-scoped and never become
allocation/revision authority.

V-6 adds `ReadyFence` to Value without changing `ImageBuffer`. Synchronous CPU
Values start Ready; a pending Value retains immutable metadata but
`buffer_handle()` and checked views reject payload access until Ready. The
source-private producer retires its mutable capability before every terminal
state, and the source-private transfer task copies a distinct CPU allocation
only as executor-queued work.

V-8 adds explicit `DeviceBackend`, `DeviceId`, `MemoryDomain`,
`StorageBinding`, producer identity, and `AccessPlan` without adding fields to
`ImageBuffer`. A Value binding may be host-visible or device-local; metadata
observation grants no pointer, and host access fails rather than starting an
implicit transfer. CPU/Metal transfer produces a distinct binding for the same
logical revision. Exact current-generation completion publishes Ready and
process residency atomically; stale completion publishes typed failure before
dependants can observe Ready. The lineage row is pretracked before coordinator
submission without a managed current identity. Accepted current publication
assigns the exact generation, including a coordinate-authorized numeric
decrease, before currentness becomes observable; a stale Run that starts later
cannot replace that exact identity. Standalone lineages separately retain
numeric-maximum ordering. Run settlement does not
itself invalidate an eligible replica, while the manager's 64-entry default
releases the lowest-revision strong native/provider owner under publication
pressure. This entry count is not device-byte or scratch admission. The current
source-private Metal path implements both buffer-to-texture upload and
texture-to-buffer download.

Issue #102 leaves this public `Value`/`BufferHandle` contract and
`ImageBuffer` unchanged. Its source-private isolated-CPU adapter accepts only
Ready, Host-visible, unquantized NativeScalar Strided DenseTensor inputs,
retains a checked read lease, and copies the exact physical storage envelope
into one invocation-local read-only shared-memory descriptor range.
`BufferHandle`, `AllocationIdentity`, `ValueRevisionId`, pointers, and leases
never cross the wire. Each output capability grants only a planned, positive,
checked, non-overlapping descriptor range. The Host binds every physical
input descriptor-range byte into its request content binding, revalidates the
returned capability after normal child exit, copies through `ValueBuilder`, and
validates the binding over the actual fresh snapshot before sealing the Host
`Value`. Descriptor-addressable padding participates in content binding.
Darwin's page-rounded POSIX
shared-memory slack remains outside every descriptor range, while its exact
physical capability size is still bound by the header and resource
declaration. No `ImageBuffer` adaptation or public memory-contract widening is
introduced. Isolated CPU protocol v2 carries the complete optional ImageFacet:
image outputs preserve axes, signed data/display windows, stable channel/group
metadata, sample domains, and color facts, while generic DenseTensor outputs
carry no ImageFacet. The Host rejects presence/identity mismatches instead of
inventing or dropping metadata.

Issue #86 / V-9 adds source-private device resource accounting without
changing `ImageBuffer` or the public operation and Host contracts. The sole
service ledger creates isolated memory/scratch accounts only for configured
non-CPU `DeviceId` values backed by executors in the fixed registry. Perlin and
CPU-to-Metal upload reserve complete native size/alignment plans before
allocation, reconcile `allocatedSize`, and commit exact actual bytes before
command submission. Persistent memory leases follow the native Value/residency
owner, while scratch leases follow exact completion; Run settlement does not
release a still-owned allocation, and the residency entry count remains a
retention bound rather than byte authority.

Issue #89 / V-12 now verifies the generic matrix without changing
`ImageBuffer`. The long-lived dependency-neutral case covers 1/3/4/8/16-channel
FP32/FP64 image Values, rank-one through rank-five FP32/FP64 latent Values,
positive padded producer layouts, negative/zero-stride immutable views,
ImageRect/TensorSlice merge, explicit CPU and injected external-device
transfer, and bounded compute-I/O retention. CPU/device transfer preserves the
complete positive producer envelope and exact logical revision in a distinct
binding; Region merge preserves logical selected/unselected elements while it
may publish a fresh contiguous allocation. Signed/zero layouts remain
immutable view facts and are rejected explicitly as transfer producer layouts.
The rank-one fixtures use a sole stride wider than the element width, and an
independent direct-offset byte oracle proves the exact storage span, active
values, and untouched padding sentinels. CPU-copy and external-device
preparation reuse one core positive, zero-offset, exact-envelope, non-overlap
validator. External rejection occurs before retaining the destination owner,
minting allocation/revision/producer facts, creating a fence, invoking the
provider, or publishing a Pending destination; the general native publisher is
not narrowed and may still publish checked signed immutable aliases.

Issue #90 / V-13 likewise leaves `ImageBuffer` unchanged while installing one
real packed path beside it. Four-bit E2M1 storage and finite-positive
row-major block scales are independent descriptor facts; version-1
`BlockedLayout` carries nibble-aligned bit strides, an absolute bit offset, and
explicit nibble order. `PackedDenseTensorView` performs checked encoded and
dequantized reads, and the bounded TensorSlice copy accepts only complete
block-aligned selections, directly copies codes/scales, and publishes a fresh
packed CPU Value. `DenseTensorView`, `ImageView`, and
`dense_tensor_element_bytes()` continue to reject Blocked FP4 rather than
pretending that one nibble is one byte.

Explicit CPU and injected external-device transfer preserve packed bytes,
unused nibble bits, descriptor/quantization/layout facts, and logical revision
without adapting through `ImageBuffer`. Formal HP memory cache can retain this
Value and exact TensorSlice validity. The image-only disk cache is an explicit
compatibility boundary: a packed, quantized, or latent formal Value fails with
`GraphError{InvalidParameter}` before executor admission, filesystem mutation,
or codec calls. No widening, metadata-only fallback, or generic durable format
occurs.

Issue #117 / V-14 also leaves `ImageBuffer` unchanged. It adds a separate
provider-defined `Value` representation whose `ProviderDefinedLayout` names
one or more checked `BufferHandle` ranges through bounded buffer envelopes.
Generic Host validation proves every index, nonzero role, offset, length, and
checked end before invoking the matching exact-generation provider. A
provider-defined Value exposes only indexed `ProviderReadLease`; each read
retains both the selected allocation and the provider generation. DenseTensor
byte/view/layout accessors and existing transfer tasks reject this
representation instead of adapting it through `ImageBuffer` or assuming one
buffer.

The V-14 pure-C definition suite receives payload only for explicit semantic
validation and canonical logical-content traversal. Property, DataSpec, and
Region evaluation sees buffer sizes and identities with null payload pointers,
and has no mapping, transfer, conversion, device, or executor authority. Atomic
provider replacement and unload remove new interpretation visibility while old
Values, reads, and provider owners retain the retiring generation and module.
Canonical ContentDigest excludes physical buffer order, padding, and offsets
when the provider emits the same logical stream. Artifact-envelope
serialization preserves metadata and unknown extension bytes, but creates no
filesystem, cache, or `ImageBuffer` persistence path.

Issue #118 / V-15 keeps that memory contract and `ImageBuffer` unchanged while
binding one optional OpenEXR codec to it. The concrete deep Layout has a
row-major `uint32` count for every logical pixel, a `uint64` prefix-offset
array of site-count plus one whose first entry is zero and whose final entry is
the declared deep-sample count, and, when samples exist, one tightly packed
FP32 sample buffer per explicit channel identity. Every offset is monotonic
and in range; every channel buffer has exactly the shared declared sample
count. Signed data and
display windows remain descriptor facts and do not become negative storage
offsets. File-channel sampling must be one-by-one, and channel names are
diagnostic only. The unchanged V-14 nonempty-envelope invariant makes a
zero shared sample total use two physical buffers only: counts and offsets.
Channel identities and roles remain in descriptor/Layout metadata, while no
sentinel payload, zero-length envelope, or fake sample identity is published.

Codec staging occurs only inside one admitted source-private adapter call. It
uses indexed `ProviderReadLease` values while translating generic buffers,
does not publish OpenEXR pointers or exception types, and constructs the
decoded result through the active registry before returning it. The provider
generation, Value, transaction token, and path copy are retained through the
complete I/O task; none becomes a public memory binding or a second ownership
authority.

V-15 still does not implement other quantization formulae or packed formats,
unaligned requantizing slices, general Map/Import providers, the remaining
provider ABI suites, a public device registry, device queue/in-flight
accounting, generic graph/cache Value persistence, deep-tiled or multipart
OpenEXR, or general named graph Value outputs. Issue #87's compute-I/O durability
decision and Issue #88's first bounded cache/codec execution vertical remain
current: the process executor retains transaction lifetime and budgets work,
but does not change `ImageBuffer` or codec ABI. The V-12 I/O observation proves
generic Value retention by an admitted task, not a lossless artifact format.
Post-publication cache outcomes and durable output remain future.
`ImageBuffer` remains the compatibility contract for private tiled writes,
existing image codecs, and public Host surfaces until DI-4; operation ABI v1
uses complete Value/grant records. The V-15 adapter does not
route its provider-defined Value through that compatibility representation.

Issue #94 keeps `ImageBuffer` and every installed memory contract unchanged.
Its source-private progressive RT branch uses
`exact_box_average_factor_four_region()` to create an aligned 512x512
RGBA FP32 preview from the original 2048x2048 source. Each 4x4 channel sum is
accumulated before one binary32 result rounding, and the caller's floating-
point environment is restored on every exit. Before the first write, a shared
source/destination owner, overlap between their checked active storage-envelope
half-open `uintptr_t` intervals, or an unrepresentable endpoint is rejected
fail-closed. This covers offset aliases under one owner and overlapping ranges
with different owners without relationally comparing unrelated pointers. The
resulting proxy storage is sealed as an immutable rank-three HWC `Value` with
its own revision, binding, allocation, `ImageFacet`, layout, and exact storage-
byte envelope. The final Value is independently computed from the original
full-resolution source.

The I2 Host records two Direct access plans to each visible preview/final Value
and requires the same revision, binding, allocation, and byte count with zero
transfer. When a configured Metal executor exists, the first acquisition uses
the process-owned registry, residency manager, and ledger to upload the tightly
strided rank-three HWC Value. The native R32 texture flattens channels into row
width only at the Metal boundary while the published device Value preserves the
original descriptor, facet, layout, logical revision, and byte envelope. A
second acquisition must reuse that exact residency without another transfer or
allocation; no readback occurs. Absence of a usable Metal executor makes only
the device component N/A and does not relax Host or no-I/O evidence.

The portable CPU allocation guarantee remains 64-byte row-start alignment.
128-byte alignment is not part of the current contract.

Separating immutable descriptors from writable payload views prevents parallel
tile callbacks from racing to replace ownership or device metadata. Keeping
the current image-only `PixelRect` view distinct from Region also prevents
private OpenCV geometry or TensorSlice reinterpretation from entering the
operation ABI.

## Implementation and Validation Entry Points

- `include/photospider/core/image_buffer.hpp`
- `src/lib/core/image_buffer_processing.hpp`
- `src/lib/core/image_buffer_storage.hpp`
- `src/lib/core/exact_box_downsample.cpp`
- `include/photospider/core/device.hpp`
- `include/photospider/memory/access_plan.hpp`
- `include/photospider/memory/buffer_handle.hpp`
- `include/photospider/memory/ready_fence.hpp`
- `include/photospider/data/value.hpp`
- `include/photospider/data/extension.hpp`
- `include/photospider/data/image_view.hpp`
- `include/photospider/data/packed_dense_tensor_view.hpp`
- `include/photospider/memory/blocked_layout.hpp`
- `include/photospider/data/region.hpp`
- `include/photospider/memory/strided_layout.hpp`
- `include/photospider/plugin/operation_plugin_api.h`
- `include/photospider/plugin/data_definition_registry.hpp`
- `include/photospider/plugin/data_provider_api.h`
- `src/lib/core/image_buffer.cpp`
- `src/lib/core/pending_value.hpp`
- `src/lib/core/value.cpp`
- `src/lib/core/extension.cpp`
- `src/lib/core/packed_dense_tensor.cpp`
- `src/lib/execution/transfer/value_transfer_task.*`
- `src/lib/execution/device/metal_device_executor.{mm,stub.cpp}`
- `src/lib/compute/execution/execution_service.*`
- `src/lib/benchmark/i2/i2_host.hpp`
- `src/lib/execution/device/device_completion.*`
- `src/lib/execution/device/residency_manager.*`
- `src/lib/plugin/data_definition_registry.cpp`
- `src/lib/adapters/openexr/openexr_deep_contract.hpp`
- `src/lib/adapters/openexr/openexr_deep_scanline_adapter.*`
- `plugins/data/openexr_deep_scanline_provider.cpp`
- `src/lib/execution/device/metal_device_executor.*`
- `src/lib/core/value_image_adapter.*`
- `src/lib/core/region.*`
- `src/lib/core/region_image_adapter.*`
- `src/lib/core/cpu_dense_image_operation.*`
- `src/lib/compute/image_buffer.hpp`
- `src/lib/adapters/opencv/buffer_adapter_opencv.*`
- `src/lib/ipc/output_store.*`
- `plugins/ops/save_op.cpp`
- `tests/unit/test_image_buffer_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/unit/test_region_contracts.cpp`
- `tests/integration/test_stride_aware_compute_paths.cpp`
- `tests/integration/test_ipc_daemon.cpp`
- `tests/integration/test_cpu_dense_tensor_image_operation.cpp`
- `tests/integration/test_packed_fp4_dense_tensor.cpp`
- `tests/integration/test_variable_sample_field_extensions.cpp`
- `tests/integration/test_openexr_deep_scanline_provider.cpp`
