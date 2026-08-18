# ADR 0008: Generic Values, Memory Bindings, and Regions Are Explicit Versioned Contracts

## Status

Accepted as the target contract for Project 4 generic data and heterogeneous
execution. The source tree now implements bounded V-2 through V-15 slices:
CPU DenseTensor/ImageView values, checked BufferHandle ownership and runtime
identity, and the public Region MVP used by dirty planning, validity, and the
core dense operation; V-5 operation-metadata routing and the bounded V-6
ReadyFence, pending CPU Value, and explicit fake-device Value-copy proof are
also current. V-7 adds one source-private process device-executor registry and
runs the repository Metal Perlin operation through its owned queue, invocation
allocator, and pipeline cache. V-8 adds explicit device/binding facts,
nonblocking access plans, revision-preserving CPU/Metal transfers, exact
process residency, Run-scoped pending-Value continuations, and stale native
completion rejection before destination readiness. `ImageBuffer`, `DataType`,
`Device`, and `ParameterMap` remain compatibility contracts at their
role-specific edges; operation plugins now use the complete pure-C ABI v1.
The unimplemented portions of this ADR remain evolution targets.

V-9 adds source-private per-device memory/scratch plans, native actual-byte
reconciliation, and leases following native Value and completion ownership
without changing those public contracts.

V-10 ratifies typed compute-I/O completion and keeps cache, Graph-document,
daemon-delivery, and durable-output authorities separate. V-11 installs the
first bounded cache/codec execution vertical through `ComputeIoExecutor`
without changing codec ABI or commit policy. V-12 adds a dependency-neutral
verification matrix for 1/3/4/8/16-channel FP32/FP64 image Values, rank-one
through rank-five FP32/FP64 latent Values, positive padding, bounded signed and
zero-stride immutable views, exact Region merge, explicit CPU/external-device
transfer, and compute-I/O retention. The matrix verifies existing contracts;
it does not widen the built-in operation, real Metal provider, or image-codec
capability surface.

V-13 adds one versioned Blocked FP4 E2M1/quantized DenseTensor vertical with
checked packed access, block-aligned TensorSlice copy, representation-
preserving transfer, memory-cache retention, and fail-closed image-disk-cache
behavior. V-14 adds a dependency-neutral provider-defined `Value` vertical:
byte-preserving Schema/Facet/Layout envelopes, multiple checked buffers, one
injected `DataDefinitionRegistry`, the exact pure-C definition-suite ABI v3,
pure property/DataSpec/Region evaluation, canonical descriptor/content/layout
SHA-256 identities, artifact-envelope round-trip, and generation-retaining
replacement/unload. It intentionally adds no access, conversion, inference,
execution, codec, OpenEXR, or operation-plugin replacement suite.

V-15 adds the optional repository OpenEXR single-part deep-scanline
provider/codec vertical. It maps explicitly identified, unit-sampled FP32
channels to `VariableSampleField + ImageFacet + DeepSampleFacet`, routes each
whole-file operation through bounded `ComputeIoExecutor`, retains registry
generation and `Value` leases, and keeps discovery and package dependency
behind the default-OFF component. Issue #118 implemented and independently
validated this bounded slice. Deep tiled, multipart, mixed shallow/deep parts,
sampled or non-FP32 channels, streaming decode/encode, broader import mapping,
and public Host/frontend provider selection remain future work. V-15 does not
add the remaining access/conversion/inference/execution suites, generic
graph/cache persistence, or operation-plugin/Host migration; DI-3 later
implemented the separately scoped operation boundary.

Issue #78 ratified this contract. Issues #79 through #90 delivered the bounded
V-2 through V-13 implementation and decision slices. Issue #117 implemented the
separate synthetic `VariableSampleField` V-14 proof without an optional codec.
Issue #118 implemented and independently validated the bounded V-15
provider/codec slice described above. These delivered slices complete the
bounded Project 4 migration tracked by Issue #77. Live Issue and Project
records remain the delivery-status authority; this ADR does not claim that PR
#116 is merged.

## Context

The current `ImageBuffer` contract is a useful two-dimensional image payload,
but it cannot be extended into the general graph value model by appending
fields. Logical meaning, physical storage, device access, readiness, cache
identity, and region reasoning have different owners and version lifetimes.
Conflating them would make a storage move change logical identity, let one
device enum imply access, make padding part of content identity, and force
future data families into one closed enumeration.

The target must support homogeneous rank-N tensors, arbitrary-channel images,
sub-byte and quantized encodings, provider-defined layouts, multiple memory
domains, asynchronous producer completion, bounded logical regions, structured
values, and variable per-site samples. It must preserve unknown valid
extensions, remain dependency-neutral, and allow provider generations to
retire safely while in-flight values, leases, queries, and callbacks still
exist.

The decision must also preserve existing ownership decisions:

- [ADR 0003](0003-process-owned-execution-resources.md) and
  [ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.md)
  keep physical execution resources, admission, ready work, and provider
  lifecycle in the injected process execution domain.
- [ADR 0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.md)
  keeps current facts, accepted decisions, evolution targets, and live delivery
  status distinct.
- [ADR 0002](0002-external-libraries-are-kernel-adapters.md) keeps optional
  libraries behind dependency-neutral provider and adapter boundaries.

## Decision

### Logical descriptors and physical bindings are separate

Every concrete `Value` has one immutable `DataDescriptor`. The descriptor
contains only logical semantics:

- exactly one versioned `RepresentationSchema` identity and canonical payload;
- zero or more versioned Facet identities and canonical payloads; and
- concrete logical dimensions, element semantics, channel, time, domain, and
  other schema-defined facts required to interpret the logical object.

It contains no allocation, plane, stride, byte offset, packing, device,
mapping, fence, or native handle. Physical facts use the separate composition:

```text
StorageBinding =
  StorageLayout
  + BufferHandle[]
  + ReadyFence
  + AccessProvider lease
```

A `Value` may own multiple authoritative bindings only when its producer
declares them equivalent for the same logical revision. Residency or low
access cost alone does not make a binding authoritative. The core validates
bounded envelopes and cross-references; the matching providers validate
schema-, Facet-, Layout-, and access-specific invariants.

### Value semantics, construction, and lifetime

`Value` is a final, copyable, immutable RAII handle implemented through PImpl.
Copying shares immutable control, allocation owners, and provider-generation
leases; it does not copy payload bytes. Consumers cannot subclass `Value` or
replace its descriptor, bindings, readiness, revision, or leases.

`ValueBuilder` is move-only and owns the only ordinary write authority for
storage under construction. It may abandon construction or seal exactly once.
A successful seal:

1. proves that the descriptor is fully concrete;
2. validates the descriptor, Layout, handles, fence, access, and provider
   envelope;
3. creates a fresh process-local `ValueRevisionId`;
4. atomically revokes every builder- or caller-held `WriteLease` and every
   public or consumer path that could acquire write authority; and
5. publishes an immutable `Value` after completing the producer-authority
   transition below.

Seal is one atomic write-authority transition. If the `ReadyFence` is already
terminal, every producer write authority must have stopped accessing storage
and retired before seal. If the fence is Pending, seal transfers the one
exclusive write authority to a private producer-scoped write capability. Only
the registered asynchronous producer, or a native owner acting as that
producer, may retain that move-only capability. Its lifetime is coupled to the
producer's terminal fence transition. It is bound to the exact
`ValueRevisionId`, provider-generation lease, and prevalidated
`StorageBinding`/Layout/`BufferHandle` envelope. It may finish only the
previously committed payload writes inside that envelope; it cannot change the
descriptor, binding set, Layout, allocation/native owner, or revision. It is
never public, copyable, or obtainable by a consumer.

The payload may still be pending behind `ReadyFence`; the descriptor may not
be pending. Pending means only that the registered producer is completing the
already committed payload through its restricted capability; it does not make
the sealed `Value` generally writable. `ImageView` and `DenseTensorView` are
checked final facades that retain the complete `Value`. Their construction
either validates the required Schema and Facets or returns a typed failure.
They never borrow a naked descriptor or allocation.

DI-2 realizes this rule for dense-image outputs through one immutable
`DenseImageOutputPlan` and one Host-owned `HostOutputBinding`. The plan freezes
the exact descriptor, layout, byte envelope, allocation alignment, output name,
and producer count before execution. The binding alone owns the `ValueBuilder`
and issues move-only whole-output or tile grants. Grant construction rejects
empty, overlapping, out-of-bounds, misaligned, or overflowed envelopes before
exposing writable bytes. A grant retirement is exact and idempotence is not
inferred: duplicate retirement, sealing with an active or omitted grant, and
publication after failure are rejected. The final successful retirement makes
all writes happen-before one seal and one named-`Value` publication; any
exception, cancellation, abandoned grant, or explicit failure instead revokes
all grants and preserves one sticky failure. This is the sole internal output
allocation and publication authority that DI-3 may project into operation ABI
v1; it is not a compatibility wrapper around `ImageBuffer`.

`StructuredValueSchema` v1 is self-contained. Its descriptor may recursively
describe fields, but one v1 `Value` does not contain runtime child `Value`
objects. Independent results use named output ports or an identity-free
`ValueBundle`. A shareable `CompositeValue` DAG, cross-value identity, cycles,
and graph persistence require a separate decision.

### Representation Schema, Facets, and extension identity

There is no closed `ValueKind` enumeration and no generic bag of optional
properties. A `RepresentationSchema` defines logical representation, such as
DenseTensor, VariableSampleField, or StructuredValue. Orthogonal versioned
Facets add image, Deep sample, color, alpha, or time meaning.

Every Schema and Facet definition has:

- a permanent 128-bit `SchemaId` or `FacetId`;
- a diagnostic name that is never authoritative identity;
- an independent structural version `{major, minor}`;
- a canonical bounded byte representation; and
- applicable validation, query, region, digest, migration, conversion,
  inference, and execution hooks.

Providers publish explicit supported version sets or ranges and explicit
migrations. Equal major numbers across unrelated identities do not imply
compatibility. When a bounded envelope is valid but its provider is absent,
the system may preserve the unknown bytes exactly. It may not interpret,
normalize, recompute, convert, region-evaluate, canonically content-hash, or
execute that extension.

The stable core knows only envelope framing, permanent identities, structural
versions, generic memory bounds, canonical payload framing, common
query/result types, and registry protocols. It does not know all future
Schemas, Facets, Layouts, devices, conversions, or kernels.

### Dense tensors, images, variable samples, and structured values

DenseTensor represents a rank-N rectangular set of homogeneous logical
elements. It implies no NCHW/NHWC order, image axes, channels, color, alpha, or
media time. A runtime descriptor has concrete rank and extents; symbolic or
unknown runtime dimensions cannot be sealed. `DataSpec`, rather than
`DataDescriptor`, may carry symbolic dimensions and constraints.

An ordinary arbitrary-channel image is:

```text
DenseTensor + ImageFacet
```

`ImageFacet` maps signed half-open x/y coordinates and an optional channel axis
to the underlying representation. It does not select planar or interleaved
storage. Signed `ImageBounds` can represent nonzero or negative data-window
origins.

`ChannelSchema` supports arbitrary channel counts, stable `ChannelId` values,
and explicit `ChannelGroupId` groups. Names are diagnostic and never imply
color, alpha, depth, or another role. `ColorFacet` and `AlphaFacet` bind
semantics to explicit groups or channels. They never infer roles from names
such as `R`, `G`, `B`, or `A`; conversion produces a new `Value`.

`TimeFacet` has a `TimeDomainId`, `TimeBase`, signed integer ticks, and
half-open `TimeRange`. A nominal rate is rational when exact or explicitly
marked approximate. Variable-frame-rate timestamps remain authoritative.

More channels do not make an image Deep. A variable number of samples at each
logical site uses `VariableSampleField`. An OpenEXR Deep logical value is:

```text
VariableSampleField + ImageFacet + DeepSampleFacet
```

Heterogeneous or independently sampled channels use a multi-plane structured
representation or a versioned extension instead of violating DenseTensor
homogeneity.

### Element meaning, encoding, and quantization

The target replaces `DataType` as a universal data contract with three
independent concepts:

- `ElementSemantics`: logical domain and interpretation;
- `StorageEncoding`: bit width, lanes, packing, byte/bit order, and physical
  encoding rules; and
- `QuantizationSchema`: scale, zero point, block/group axes, calibration, and
  schema-defined quantization parameters.

Support is classified independently as describable, executable, and
convertible. Describable means the descriptor and storage envelope are valid;
executable means a selected kernel supports the requested capability;
convertible means an explicit registered conversion exists. No path silently
changes meaning, encoding, quantization, channel roles, color, alpha, or time.
An explicit conversion creates a new `ValueRevisionId`.

### Buffer handles, layouts, leases, and write safety

An allocation control block owns native allocation identity, provider/device
state, release behavior, and lifetime. `BufferHandle` is an immutable copyable
view of a checked byte range in that allocation. It contains neither a general
raw pointer nor a copyable mutable flag.

One binding may use multiple handles, and handles may refer to disjoint or
overlapping ranges only when Layout and access rules permit it. Core validation
uses checked arithmetic to prove that every declared envelope is within its
handle.

Public builder and consumer payload access requires:

- `ReadLease`, retaining allocation, provider generation, mapping/import state,
  visibility obligations, and immutable access; or
- `WriteLease`, additionally proving exclusive builder authority and a valid
  non-overlapping writable Layout.

A Pending producer-scoped write capability is the only seal-time exception to
ordinary lease access. It carries the same checked, non-overlapping envelope
proof but is private to the registered producer or a native owner acting as
that producer and cannot be requested through a sealed `Value`.

A sealed `Value` cannot issue `WriteLease`, and consumer writable access is
always rejected. Direct CPU pointers, mapped pointers, imported resources, and
transfer destinations exist only within the corresponding lease or private
producer capability and access-provider contract.

The first core Layout families are:

- `Strided`: byte offset, shape-compatible plane mapping, and signed byte
  strides;
- `Blocked`: explicit block/tile geometry and packing; and
- `ProviderDefined`: versioned opaque payload plus a mandatory generic bounded
  buffer envelope.

Strided reads may use positive, negative, or zero strides. Negative and zero
strides are read-only in v1. Writable access must prove in-bounds,
non-overlapping addresses for the requested write region; failure to prove
that property rejects the lease. A provider-defined Layout never bypasses core
bounds: it names each handle and byte envelope it may touch.

### Device identity, access, residency, readiness, and visibility

The target separates:

- `DeviceBackend`, a stable backend family such as CPU, CUDA, Metal, or Vulkan;
- `DeviceId`, a process-local identity for one concrete device; and
- `MemoryDomain`, a versioned allocation domain such as host, pinned host,
  device-local, shared, or imported memory.

None of these implies accessibility. An `AccessProvider` returns an explicit
`AccessPlan`:

```text
Direct | Map | Import | Transfer | Unsupported
```

The plan identifies source binding, target capability, required readiness,
visibility work, leases, resource demand, and the resulting binding or access
scope. Device, Layout, memory-domain, residency, and access constraints belong
to `KernelCapability`, not `DataSpec`.

A `Value` owns authoritative bindings. The process-owned `ResidencyManager`
may own extra replicas keyed by `ValueRevisionId`; those replicas are neither
serialized into `Value` nor treated as authority. A stale replica cannot
satisfy a newer revision.

`ReadyFence` is an immutable copyable observer. `FenceCompleter` is the
move-only terminal-publication capability; it does not by itself grant payload
write access. Exactly one terminal transition is allowed:

```text
Pending -> Ready
        -> Failed
        -> ProducerCancelled
```

Dropping an unresolved completer publishes `ProducerCancelled`. `poll` is
nonblocking. Waits are scheduled asynchronously through the owning execution
or device mechanism; CPU workers do not block on device fences. Cancelling a
waiter does not mutate the shared fence or cancel the producer.

Before publishing Ready, Failed, or ProducerCancelled, the producer must stop
all payload access and release or revoke its producer-scoped write capability.
All producer writes and that capability's retirement happen-before observers
can see the terminal transition. Publishing a terminal state while the
capability can still access storage is invalid.

`Ready` reports producer completion only. It does not establish host mapping,
cache coherency, queue-family ownership, or consumer visibility; those duties
belong to `AccessPlan`. Pending, Failed, and ProducerCancelled permit immutable
metadata and diagnostics but cannot yield a consumer `ReadLease` or
payload-visible access. After Ready, consumer reads still require the selected
`AccessPlan` to discharge its visibility obligations before issuing the
`ReadLease`. Fences, pending waits, access scopes, private producer
capabilities, and native owners retain the defining provider-generation lease.

The implemented V-6 subset realizes the fence state machine, executor-enqueued
waits, Value read gating, and a source-private pending CPU producer. Its
source-private `ValueTransferTask` copies one validated CPU envelope only after
source readiness and publishes a distinct destination.

The implemented V-7 subset adds a separate source-private
`DeviceExecutorRegistry` under `ExecutionService`. In the enabled repository
Metal-plugin profile, its Apple executor owns one native device and queue, one
callback-scoped texture/buffer allocator, and a validated persistent pipeline
cache. Reserved-start work invokes the
Metal Perlin provider inside that borrowed context and returns an independent
CPU compatibility image.

The implemented V-8 subset adds dependency-neutral `DeviceBackend`,
`DeviceId`, `MemoryDomain`, `StorageBinding`, producer identity, and exhaustive
`AccessPlan` outcomes. `BufferHandle` may retain a source-private native owner
without exposing its handle; non-host-visible access throws instead of mapping,
waiting, or transferring. CPU-to-CPU, CPU-to-Metal, and Metal-to-CPU transfers
publish distinct bindings that preserve one logical `ValueRevisionId`. The
process-owned `ResidencyManager` admits complete Run/task/producer/revision/
binding identities and linearizes current-generation validation, producer
Ready publication, and replica insertion under one lock. For a coordinator-
managed lineage, an updated accepted current identity therefore either makes
an old callback typed-Failed before destination Ready, or follows a completion
published against the then-current exact generation. Numeric generation
magnitude does not establish managed currentness; standalone lineages retain
numeric-maximum order. Mismatched and duplicate identities cannot consume or
republish another admission. Pending operation
Values keep their Run unsettled and release dependants only through the same
`ExecutionService` ready store after terminal success. The Metal Perlin path
encodes an explicit texture-to-shared-buffer blit, installs the completion
handler, commits, and returns without `waitUntilCompleted` or `getBytes`.
V-9 keeps the Host `ResourceVector` unchanged and adds isolated immutable
memory/scratch accounts for each configured non-CPU `DeviceId`. Perlin and
CPU-to-Metal upload query native heap size/alignment and reserve their complete
plans before allocation. They audit `allocatedSize`, return unused plan bytes,
and commit exact actual bytes before command submission. The memory lease is
owned by the same type-erased native owner that `BufferHandle` copies and
residency retains; scratch is owned by the exact completion object until
terminal native handling. Residency remains a count-bounded retention owner,
not a byte-budget authority.

The current-generation handoff is staged rather than allocating in the
coordinator critical section. Kernel pretracks a zero-generation lineage row
before submitting publication. Only an accepted current candidate assigns the
exact published generation through a no-throw callback immediately before
coordinator currentness becomes observable. A rejected or born-stale candidate
leaves the row unchanged, and a stale Run that starts after another accepted
identity becomes current cannot overwrite that exact managed identity,
regardless of numeric generation direction. Standalone rows separately retain
numeric-maximum order. The prepared candidate owns a
compute-request-lane reserved ticket before this fallible pretracking step.
Graph close therefore drains and joins that lane before retiring the exact
Graph's lineage rows, preventing a paused caller from recreating maintenance
state after retirement.

A `ComputeRun` retains request-local immutable Values and their authoritative
bindings. Settlement accounts for every output's terminal fence state,
provider-generation lease, access obligation, and ResourceLedger release. A
failed fence may settle the Run with a typed failure and release request
accounting while callbacks, waits, or access leases keep the retiring provider
and native owners alive. Closing the Run releases its Value handles but neither
destroys an in-flight owner nor prevents an eligible process-owned residency
replica from remaining under its exact `ValueRevisionId`. This rule does not
move Run, dispatcher, ledger, commit, or shutdown authority away from the
owners defined by ADR 0007.

### Bounded logical regions

`RegionSet` is a bounded, extensible disjunction of conjunctions over explicit
`RegionDomainKey` values:

- atoms in one clause combine by AND;
- clauses combine by OR; and
- every atom names its logical coordinate domain.

`Whole` is one empty clause. `Empty` is zero clauses. Intervals are half-open.
The MVP supports Whole, Empty, `ImageRect`, `TensorSlice`, and at most one
nonempty clause. Additional atom types and multiple clauses are versioned
extensions.

Union, intersection, difference, projection, and transformation consume an
explicit complexity budget and return one of:

```text
Exact(RegionSet)
ConservativeSuperset(RegionSet, reason)
Unknown
Unsupported
TooComplex
```

The latter three are outcomes, not fake regions. Hull or Whole fallback is
legal only as a labelled `ConservativeSuperset`; each caller decides whether
the approximation is safe for planning, invalidation, or execution.

The implemented V-4 subset installs this value/algebra contract with fixed
built-in image and dense-tensor domain keys, signed 64-bit `ImageRect`
intervals, rank-general unsigned 64-bit `TensorSlice` intervals, a hard limit
of eight atoms in the single nonempty clause, and explicit caller budgets.
Dirty source facts, per-node affected work, edge mappings, HP/RT validity, and
the core dense operation retain normalized `RegionSet`. Current image tiling,
ImageBuffer helpers, Host/IPC inspection, and operation ABI v1 adapters retain checked
derived `PixelRect` projections. RT is image-only; TensorSlice is HP
monolithic work and is never reinterpreted as two-dimensional geometry.

### DataSpec, capabilities, properties, and output inference

`DataSpec` describes a set of acceptable concrete descriptors. It may contain
symbolic dimensions, ranges, Schema/Facet version constraints,
element/quantization predicates, and provider-evaluable conditions.

For producer set `P` and consumer set `C`:

- `P` is statically compatible when `P` is a subset of `C`;
- it is incompatible when their intersection is empty;
- nonempty partial overlap requires an explicit runtime guard; and
- missing provider logic yields `CannotEvaluate`.

Compatibility never inserts a conversion. Graph planning selects an explicit
conversion operation when needed.

A pure nonblocking property query returns exactly one
`PropertyQueryResult<T>` state:

```text
Available(T)
NotApplicable
Unknown
Deferred
MissingProvider
UnsupportedSchemaVersion
InvalidDescriptor
```

Queries perform no IO, payload access, mapping, transfer, allocation-sized
compute, or device work. Work required to resolve `Deferred` is an explicit
operation or scheduled prepare step.

Output description has three stages:

1. static inference maps input `DataSpec` plus configuration to output
   `DataSpec` without reading payloads;
2. pure concrete inference maps concrete descriptors plus configuration to
   `Exact(DataDescriptor)` or `Deferred`; and
3. scheduled prepare/execute performs content-, IO-, or device-dependent work.

An operation may start scheduled work with a deferred descriptor, but cannot
seal the output until the descriptor is concrete. A sealed output may be
payload-pending behind its fence. Named operation outputs are named `Value`
objects; after migration `ParameterMap` remains configuration-only.

### Error model and provider ABI

Queries use the typed observation states above. Failing operations use
`Result<T>` or `Status` with stable categories and owned diagnostics. Pure-C
provider callbacks and asynchronous device/IO completions never propagate C++
exceptions. Provider boundaries translate resource exhaustion to
`resource_exhausted` and unknown exceptions to a host-owned internal error
while retaining the provider lease.

Schema, Facet, Layout, access, conversion, inference, query, region, digest,
and execution provider suites use a separately versioned pure-C provider ABI
v3. Records use fixed-width scalars, explicit sizes/kinds/versions, bounded
byte/string views, opaque handles, host-owned output storage, status returns,
and zero-required reserved fields. They expose no STL type, exception, RTTI
object, virtual class, allocator owner, `Value` PImpl, native owner reference,
or mutable Host registry.

In the implemented V-14 definition suite, borrowed byte views are inputs only.
Diagnostic and BYTES-property records carry scalar lengths, and the provider
copies complete fields through a callback-local Host output sink while its
source storage is alive. The sink checks channel use, pointer/count framing,
duplicates, and fixed bounds before dereference, then owns the copied bytes
independently of callback, thread, and generation lifetime. For a canonical
nonempty Exact TensorSlice, the Host also requires the provider's selected-site
count to equal the checked `uint64_t` product of every half-open axis length;
overflow or mismatch is a typed invalid provider result.

The C++ SDK provides RAII wrappers around those suites without changing their
wire layout or authority. Exact record layout, limits, calling convention, and
callback inventories must be frozen and independently reproduced before
implementation.

The injected process composition owns one `DataDefinitionRegistry` with
separate typed Schema, Facet, and Layout tables plus one provider-generation
table. It is not a global or function-static singleton. For one exact typed
identity and structural version, at most one definition provider is active;
multiple execution kernels may support that logical definition on different
capabilities.

Published definitions and kernel bindings are immutable generation owners.
Callers retain generation leases through validation, query, inference, access
planning, invocation, result conversion, and provider-created owner lifetime.
Replacement prepares a complete candidate, publishes atomically, and moves the
old generation through:

```text
Active -> Retiring -> Unloaded
```

Retiring rejects new leases but remains mapped until all existing leases and
provider-created owners retire. Failed preparation or publication preserves
the prior generation.

Policy plugin C ABI v1 is independently versioned and is not renamed to v3.
The process execution domain, ResourceLedger, ready store, and policy authority
defined by ADR 0007 remain unchanged.

### Runtime identity, digests, persistence, and cache authority

Every successful seal creates a fresh process-local `ValueRevisionId`, even if
descriptor and content equal an earlier value. Persistent and comparable
identities remain distinct:

- `DescriptorDigest`: canonical logical descriptor envelope;
- `ContentDigest`: schema-defined canonical logical content stream;
- `StorageLayoutDigest`: canonical physical Layout description without native
  allocation identity; and
- `ArtifactId`: identity of one persisted manifest or artifact version.

Each digest carries an algorithm tag. `ContentDigest` may be `Deferred`. It
excludes device identity, allocation identity, fences, padding, physical
stride, and replica state. Schema providers define canonical traversal so
equivalent logical values hash equally across permitted physical layouts.
Because the frozen content field stores its byte length before its payload,
the Host measures one deterministic provider traversal with checked
`uint64_t` accumulation, writes the canonical field header, and repeats the
same active generation under the same immutable Value view and payload leases
to feed SHA-256 incrementally. Each traversal has independent callback-local
diagnostic state. Providers must reproduce the same logical byte sequence;
callback chunk boundaries do not affect identity. The Host rejects malformed
pointer/count pairs, measurement overflow, sticky sink failure, and measured/
hashed count drift. It owns no payload-proportional staging and applies no
arbitrary 64 MiB content ceiling; only the frozen SHA-256 length framing limits
the stream.

Persistence has four layers:

1. graph documents contain operation configuration, named ports, and
   `DataSpec` constraints;
2. descriptor envelopes contain Schema/Facet identities, structural versions,
   canonical payloads, and unknown extension bytes;
3. artifact/cache manifests contain descriptor/content/layout digests,
   `ArtifactLayout`, content-addressed chunk references, codec/provider
   metadata, and `ArtifactId`; and
4. runtime state contains bindings, handles, native/allocation/device identity,
   fences, leases, access plans, and residency replicas, and is never
   persisted.

Schema, Facet, Layout, provider ABI, graph document, artifact manifest, codec,
and digest algorithm versions are independent axes. Unknown valid extension
payloads are preserved byte-for-byte through descriptor and artifact
read/write cycles; they are not normalized without their provider.

The current formal HP cache and RT transient state ownership remain in force
until implementation slices migrate them. The target does not allow a second
cache authority or promote a residency replica into logical or persistent
authority.

### Complete public migration without permanent shims

The target installed surface is organized under:

```text
include/photospider/core/
include/photospider/data/
include/photospider/memory/
include/photospider/plugin/
```

The final replacement is:

```text
ImageBuffer     -> Value + ImageFacet + ImageView
PixelRect       -> RegionSet atom ImageRect
Device          -> DeviceBackend + DeviceId + MemoryDomain
Operation result -> named Value outputs
ParameterMap    -> configuration only, never a data payload
```

Repository-owned operations and providers migrated first. Owned adapters,
tests, installed consumers, and documentation then migrated in dependency
order. DI-3 installed the separately versioned pure-C operation-plugin ABI v1
accepted by ADR 0012 and deleted the predecessor entry point, SDK, fixtures,
and package surface in the same breaking change. DI-4 still owns final public
Host/IPC/worker/durable/codec/CLI ImageBuffer migration.

The final state has no permanent compatibility wrapper, alias class, duplicate
old/new API, forwarding header, dual loader, predecessor shim, or dual
descriptor/cache/ABI authority. Temporary edge adaptation may exist only
inside an explicitly bounded implementation slice and must be deleted by that
slice's completion boundary.

### Verification boundaries

Issue #78 changes architecture and documentation only.

The first implementation chain remains issues #79 through #90. Each issue is a
separately testable vertical slice and must consume this contract without
silently narrowing it.

Issue #117 implements the dependency-free synthetic `VariableSampleField`
V-14 slice. Its durable tests prove registration, unknown byte preservation,
descriptor and Layout validation, multi-buffer binding,
Region/DataSpec/query behavior without payload authority, independent exact
canonical digests, callback-local diagnostic/property copy-out and bounds,
rank-general Exact TensorSlice count verification, generation leases, atomic
hot replacement, and unload without OpenEXR. Content-digest coverage includes
a generated stream beyond the former 64 MiB ceiling with an independent exact
SHA-256 vector, equivalent streams with different callback chunk boundaries,
and sticky malformed/overflowing sink failures while preserving the existing
frozen golden identity. The shipped ABI v3 is deliberately the definition
suite only; the broader access/conversion/inference/execution suites in this
target remain future generations or slices.

The implemented optional OpenEXR V-15 slice supports only complete single-part
deep-scanline read/write. It maps to
`VariableSampleField + ImageFacet + DeepSampleFacet` and keeps OpenEXR types,
headers, links, symbols, codec/IO, and package requirements provider-only.
Deep tiled, multipart, mixed shallow/deep parts, sampled or non-FP32 channels,
streaming decode/encode, broader import mapping, and public Host/frontend
provider selection remain later work. With the option disabled, OpenEXR is
absent from the kernel, public ABI, dependency-disabled product, and transitive
install dependencies.

## Consequences

- Logical identity survives storage movement, repacking, transfer, and
  additional residency.
- Unknown valid extensions can be retained without pretending they are
  executable or queryable.
- Memory safety, exclusive writes, readiness, visibility, and provider lifetime
  become explicit contracts instead of conventions around raw pointers.
- Region and compatibility uncertainty remain typed outcomes; callers cannot
  silently substitute Whole, a hull, or a conversion.
- The pure-C provider ABI and generation leases permit controlled replacement
  without exposing C++ layout or exception ownership across DSOs.
- The design adds explicit schemas, Facets, registries, leases, and result
  states. This complexity is accepted because the omitted distinctions are
  correctness and lifetime boundaries, not optional metadata.
- Unimplemented target behavior does not become current merely because this
  ADR accepts it; each later slice must update code, durable tests,
  current-fact documentation, and installed contracts together.

## Rejected Alternatives

### Expand ImageBuffer and DataType indefinitely

Rejected because rank, variable samples, sub-byte packing, quantization,
logical meaning, storage, and device access have independent invariants and
version axes.

### Use one ValueKind enum with optional fields

Rejected because extension identity would depend on core releases, invalid
field combinations would proliferate, and unknown payloads could not be
preserved faithfully.

### Treat Device or residency as accessibility

Rejected because backend family, concrete device, allocation domain,
visibility, and actual consumer access are different facts.

### Store one bounding PixelRect and widen silently

Rejected because it loses tensor and sparse logical domains and conceals
approximation from callers.

### Reuse the provisional C++ operation registration generation with new Value objects

Rejected because a C-linkage entry symbol does not stabilize C++ layout,
allocators, exceptions, RTTI, standard-library ownership, or toolchain ABI.

### Make OpenEXR the first extensibility proof

Rejected because a codec dependency could conceal core registry, persistence,
and memory-envelope flaws and would violate the dependency-neutral foundation.

## Relationship to Current Facts and Evolution Target

The following maintained documents remain authoritative for current behavior:

- [Kernel Data Model](../kernel-architecture/Data-Model.md);
- [ImageBuffer Memory Contract](../kernel-architecture/ImageBuffer-Memory-Contract.md);
- [Plugin ABI](../kernel-architecture/Plugin-ABI.md); and
- [Kernel Cache Model](../kernel-architecture/Cache-Model.md).

The [general data and regions roadmap](../roadmap/Kernel-Evolution.md#general-data-and-regions)
is authoritative for the accepted target and implementation dependency order.
Live issue and Project state remain authoritative for delivery status. Neither
this ADR nor the roadmap promotes an unimplemented target object into current
runtime documentation. The DI-2 output plan, binding, grant, and publication
authority described above is implemented current behavior; operation ABI v1
and the final Host/IPC/worker/durable/CLI boundary migrations remain later
slices.
