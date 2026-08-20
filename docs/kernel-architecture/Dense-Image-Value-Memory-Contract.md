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
CPU bindings retain the positive power-of-two alignment guaranteed for the
selected range; checked subranges reduce that guarantee when their offset
requires it. Alignment is a physical reconstruction fact, never descriptor or
content identity.
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
Capture records each CPU binding's guaranteed alignment. Version 1 accepts
positive power-of-two requirements through 4096 bytes, aligns every archive
span independently, and reconstructs each Strided, Blocked, or
provider-defined buffer with that exact requirement.
Finite binary32 and binary64 metadata is encoded from its numeric IEEE-754
sign, exponent, and fraction into canonical little-endian words rather than
from native object bytes or word order. Signed zero has the single `+0` wire
spelling. Decode rejects negative-zero and non-finite spellings, and the codec
fails at compile time when the host does not provide the exact supported
IEC 559 profiles, including subnormals.

Payload bytes stay outside JSON and control frames. IPC OutputStore stages all
buffers privately and publishes the complete metadata manifest last. Worker
protocol v3 transports metadata and data-plane references, not bulk bytes in
control frames. Durable manifests bind the same records to stable artifact and
commit identities while preserving manifest-last, barrier, replay, deletion,
and fail-stop ordering. Durable restart applies small fixed manifest and Job-
record limits before allocation; it checks each archive's frozen bound,
remaining tenant-retention quota, manifest-declared exact length, physical
length, and non-sparse storage before allocating or reading payload bytes.

Every decode is transactional. It checks framing, versions, bounds, canonical
ordering, descriptor/Layout digests, owner joins, exact payload lengths,
SHA-256, and local built-in or provider validation before publishing anything.
Reconstruction creates fresh allocation, Value revision, producer, fence, and
local binding identities. Aligned CPU deleters retain the matching delete
alignment, while provider Values and indexed leases retain the exact validating
generation and module lifetime. A failure, including `std::bad_alloc` during a
later buffer allocation, unwinds every earlier local owner and leaves no
partial result, formal cache mutation, receipt, quota credit, or dependent
release.

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
unquantized, Strided ordinary image Values. A whole-Value view rejects width
or height outside OpenCV's `int` extent before any narrowing, address lookup,
or matrix-header construction. An `InputTile` view instead validates the
source Value, ROI containment, and the ROI's own representable extent, then
constructs the local zero-copy matrix from the ROI size and exact first-channel
address without constructing the full matrix. For an otherwise valid one-row
matrix, a zero row stride is passed as OpenCV `AUTO_STEP`, so the matrix uses
its active row byte count; a negative stride is likewise mapped to that count.
Only a positive padded stride preserves a distinct source-step value as
`cv::Mat::step`; a positive tight stride already equals the active row byte
count.
Thus a small representable tile can view an oversized zero-stride logical image
with the exact ROI address, but its matrix step is the active row byte count.
Padded stride, zero-based OpenCV metadata, and signed Value origins remain
distinct concerns. An external-data `cv::Mat` header borrows the payload and
retains neither the Value nor a ReadLease. Whole-Value callers must keep the
supplied Value alive for every matrix access; an InputTile matrix may be used
only synchronously within its callback while the borrowed Value owner is
alive. Mutable matrices remain scoped to an exclusive Host output grant.
OpenCV decode preserves supported 8/16-bit code values and assigns an explicit
zero-origin data window because common OpenCV metadata has no signed-window
authority. Encode uses a closed
extension/depth/channel matrix: JPEG is
unsigned 8-bit with one or three channels; PNG/TIFF/JPEG 2000 accept declared
unsigned 8/16-bit one/three/four-channel combinations; BMP/WebP/Netpbm retain
their narrower declared subsets. Signed and floating matrices are rejected
before `cv::imwrite`, so OpenCV cannot silently fall back to CV_8U. The OpenCV
path never handles `.exr`.

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
rules. Exact endpoints and equal domains bypass arithmetic. Finite direct spans
first form their quotient. When it is finite and nonzero, one endpoint-relative
source distance is fused directly with the destination endpoint closest to
zero; a symmetric destination uses a stable source midpoint when available. If
either direct span overflows, the source and destination endpoints are
independently power-of-two normalized. Their bounded span quotient plus exact
exponent difference derives a candidate scale without forming either original
span. A normal derived scale multiplies an original, unscaled finite endpoint
or midpoint distance directly. A zero, subnormal, or infinite derived scale
falls back to an endpoint-relative fraction plus fused destination
interpolation. On the normal-derived-scale path, only interval endpoints are
normalized; the representable small input displacement remains unscaled until
the fused map. Forward and precision-reverse maps preserve
same-sign, cross-zero, narrow-subnormal, ratio-underflow, and cross-zero
overflow-span cases without requiring `long double` to be wider than binary64
or creating an avoidable infinity, NaN, zero radius, rounded midpoint ratio, or
premature zero. Precision Reject still compares the working affine result with
exact destination storage and exact reverse mapping; it does not pre-round a
wider `1/3` to make FP64 narrowing appear exact.
A binary64 spelling constructed as an extreme midpoint may already be rounded
before the working-type calculation. Its portable oracle therefore uses Allow
to assert the nearest destination storage; Reject success vectors use provably
exact and reversible positions such as endpoints and `0.5`, without assuming
that `long double` is or is not wider than binary64.
There is no hidden 255/65535 arithmetic, color transform, channel-role
inference, or missing-metadata fallback. Equal endpoint/storage identity reads
integer domains with type-aware comparison and copies each in-domain native
sample without floating promotion, preserving `int64_t`/`uint64_t` values
around `2^53` and at their extrema. A non-identity wide-integer conversion is
rejected before affine arithmetic on platforms whose `long double` cannot
prove exact source promotion; the final floating-to-integer cast also uses
open upper bounds so rounded `INT64_MAX`/`UINT64_MAX` endpoints can never
authorize an out-of-range cast.

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
The later-buffer artifact regression uses a BUILD_TESTING-only source-private
runtime failpoint immediately before the selected `BufferHandle::ControlBlock`
allocation. Production builds compile no test-access seam, and the test does
not replace process or shared-library global allocation symbols.
The cross-zero overflow-span regression likewise uses a BUILD_TESTING-only,
source-private, thread-local scope to select binary64 affine working arithmetic
while still calling public `convert_dense_image_samples`. Nested scopes restore
their prior mode, concurrent threads remain independent, the header is not
installed, and production builds compile neither the selector nor its branch.
