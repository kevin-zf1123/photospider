# Dense Image Value Memory Contract

This document defines the current memory, metadata, publication, and external
artifact contract for ordinary dense images. A dense image is one built-in
`Value` with a `DenseTensorDescriptor`, complete `ImageFacet`, explicit Layout,
one or more checked `BufferHandle` bindings, and a `ReadyFence`. There is no
second image value, compatibility snapshot, storage enum, or native-context
carrier.

The generic contracts in [Data Model](Data-Model.md) remain authoritative.
This document specializes them for ordinary images and their Host, codec,
cache, IPC, worker, and durable boundaries. OpenEXR Deep remains a separate
provider-defined variable-sample contract.

## Logical descriptor and image interpretation

`DenseTensorDescriptor` owns rank, positive concrete shape,
`ElementSemantics`, `StorageEncoding`, and optional quantization. It does not
own coordinates, physical strides, device placement, readiness, sample
meaning, color meaning, or observed statistics.

Every ordinary image has one `ImageFacet` with explicit, distinct x/y axes and
an optional distinct channel axis. Axes not assigned to x, y, or channel are
singleton in the current built-in image view. The facet contains:

- a required signed half-open data window whose spans exactly equal the
  descriptor's x/y extents;
- an optional independent signed half-open display window;
- an optional stable channel and group schema;
- an optional versioned sample encoding/domain declaration; and
- optional versioned color interpretation.

The data window is the logical pixel-coordinate authority. The display window
is presentation metadata and grants no payload access. `ImageView` exposes
both zero-based storage coordinates and checked signed logical coordinates;
logical access subtracts the data-window origin only after containment checks.

Diagnostic channel/group names never select semantic roles or enter semantic
equality. Stable `ChannelId` and `ChannelGroupId` records do. Observed extrema,
histograms, NaN/Inf counts, and other statistics never become descriptor,
Facet, or content identity.

## Layout, binding, and ownership

Ordinary built-in images use a validated whole-byte `StridedLayout`.
Descriptor shape and element width are independent from byte strides and
offset. Immutable Values may retain checked positive, zero, or negative
strides; writable producer builders require a non-overlapping positive layout
and exact storage envelope.

`BufferHandle` is a checked immutable byte range over one explicit
`StorageBinding`. It exposes no raw pointer or arbitrary context payload.
Host access occurs only through retaining read leases or an exclusive
producer/grant write lease. Source-private device Values retain explicit
`DeviceBackend`, `DeviceId`, `MemoryDomain`, native owner, and byte range;
device-to-Host transfer creates a distinct physical binding without changing
the logical descriptor.

`ValueBuilder` owns mutable CPU storage until its sole write lease is released
and `seal()` succeeds. Sealing validates descriptor, Facet, Layout, binding,
storage envelope, and producer state before publishing a fresh immutable Value
revision. `PendingValuePublisher` and device publication follow the same
single-authority rule: terminal failure clears private producer access, and a
successful Ready publication cannot be mutated afterward.

## Readiness and metadata-only inspection

Readiness belongs to `ReadyFence`, not to the image descriptor or artifact.
Pending, Ready, Failed, and ProducerCancelled are closed states. Payload views,
buffer leases, codec reads, and content digest computation require Ready.

Host metadata inspection copies descriptor, Facets, Layout summaries, buffer
envelopes, readiness snapshot, producer identity, optional canonical digests,
and bounded identity-independent statistics references. It does not acquire a
payload lease, map a device, wait on a fence, compute a digest, or schedule
statistics. Therefore non-Ready outputs remain inspectable without weakening
payload access rules.

## Named Host outputs

Embedded and IPC Hosts return canonical ordered named Values. Output names are
bounded, nonempty, unique, and sorted; `image` is the conventional ordinary
image output name but is not a separate result type. Successful result
delivery requires every declared output to be present and Ready. Failure owns
one bounded `OperationStatus` and publishes no partial Value set.

Formal, dirty, tiled, and real-time paths publish the same named Value shape.
Dirty execution may derive checked storage-relative `PixelRect` work from a
signed data window, but commits retain the original logical window and Value
authority. Resize/downsample scratch Values are callback-local and never enter
formal cache state as alternate snapshots.

## Portable Value artifacts

External and durable boundaries use `ValueArtifactEnvelope` version 1 and
canonical named artifact sets. The envelope preserves the complete built-in
descriptor, ImageFacet, Layout, ordered buffer roles/spans/alignment, payload
digests, optional content digest, and bounded statistics references. Store
owners wrap it with their own artifact, commit, slot, attempt, lease, quota,
and path authority; those facts do not become Value identity.

Payload bytes stay outside JSON and control frames. IPC OutputStore stages all
buffers privately and publishes the complete metadata manifest last. Worker
protocol v3 transports metadata and data-plane references, not bulk bytes in
control frames. Durable manifests bind the same records to stable artifact and
commit identities while preserving manifest-last, barrier, replay, deletion,
and fail-stop ordering.

Every decode is transactional. It checks framing, versions, bounds, canonical
ordering, descriptor/Layout digests, owner joins, exact payload lengths,
SHA-256, and local built-in or provider validation before publishing anything.
Reconstruction creates fresh allocation, Value revision, producer, fence, and
local binding identities. A failure leaves no partial result, formal cache
mutation, receipt, quota credit, or dependent release.

## Cache and persistence

The memory cache retains exact immutable Values and validity facts. Eligible
disk-cache/save operations capture a portable Value artifact directly; they do
not derive a second image representation. Unsupported packed, quantized,
device-only, provider-missing, or non-Ready inputs fail closed according to the
explicit codec or artifact contract.

Artifact identity, logical content digest, Value revision, allocation identity,
graph revision, and statistics-cache identity are separate. Replay may retain
the same artifact and content identity while necessarily creating fresh
runtime identities.

## OpenCV and ordinary OpenEXR adapters

The public OpenCV adapter accepts only Ready, Host-readable, whole-byte,
unquantized, Strided ordinary image Values. Borrowed read-only matrices retain
the source Value; mutable matrices are scoped to an exclusive Host output
grant. OpenCV decode preserves supported 8/16-bit code values and assigns an
explicit zero-origin data window because common OpenCV metadata has no signed
window authority. It never handles `.exr` paths.

The optional ordinary OpenEXR codec accepts one single-part scanline image.
It preserves independent signed data and display windows. Uniform UINT and
FLOAT channels retain 32-bit storage; HALF samples are promoted exactly to
FP32 because the built-in tensor contract has no binary16 storage encoding.
Mixed channel storage, sampled channels, tiled/deep/multipart input, missing
explicit encode display windows, and implicit numeric conversion are rejected.

OpenEXR Deep does not use this ordinary path. It remains one provider-defined
multi-buffer `VariableSampleField` Value with ImageFacet, DeepSampleFacet,
counts, prefix offsets, and one sample stream per explicitly mapped channel.
Zero or variable samples are never padded into a DenseTensor.

## Explicit sample conversion

Storage never implies sample meaning. Codec and CLI conversion uses one
`SampleConversion` containing explicit source/destination `SampleEncoding`,
finite inclusive `SampleDomain`, destination semantics/storage, and closed
policies for out-of-domain input, clamp, integer rounding, NaN/Inf, and
precision loss.

Identity conversion performs no scaling. Semantic conversion applies the
declared affine mapping only after source-domain rejection or clamp, then
applies the selected rounding, representability, non-finite, and precision
rules. There is no hidden 255/65535 arithmetic, color transform, channel-role
inference, or missing-metadata fallback.

## Implementation and verification map

Primary contracts and implementations:

- `include/photospider/data/{value,image_metadata,image_view}.hpp`
- `include/photospider/data/{sample_conversion,value_artifact}.hpp`
- `include/photospider/host/{host,value_result,value_artifact_result}.hpp`
- `src/lib/core/{value,sample_conversion,value_artifact}.cpp`
- `src/lib/adapters/opencv/{value_adapter_opencv,image_artifact_codec_opencv}.*`
- `src/lib/adapters/openexr/openexr_dense_image_codec.*`
- `src/lib/adapters/openexr/openexr_deep_scanline_adapter.*`
- `src/lib/ipc/`, `src/lib/server/worker/`, and `src/lib/server/state/`

Long-lived tests cover Value construction, signed coordinates, sample
conversion, artifact reconstruction, Host results, IPC leases, worker/durable
replay, OpenCV lifetime, ordinary OpenEXR round trips, and provider-defined
Deep behavior. Source-residue searches are migration evidence only and are not
registered as CTest or CI behavior tests.
