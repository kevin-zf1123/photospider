# Kernel Data Model

This document describes the graph and node data structures used by the current
kernel. `GraphDefinition` is the detached persistent document value;
`GraphModel` and `Node` are private backend runtime state, not shared public
contracts. Frontends use `ps::Host` values, operation plugins use the operation
SDK, and schedulers receive only ready-task metadata. This document explains
the internal behavior those boundaries ultimately operate on.

## GraphModel

`GraphModel` is the in-memory state for a graph. Each `GraphRuntime` owns one
`GraphModel`.

Important fields:

| Field | Meaning |
| --- | --- |
| private node storage | Map from node id to `Node`, accessed through `GraphModel` lookup, iteration, and mutation helpers. |
| topology adjacency index | Incoming and outgoing `GraphTopologyEdge` maps for image and parameter edges, keyed by stable node id. |
| `cache_root` | Resolved root directory for this graph's disk cache files. |
| `timing_results` | Latest timing summary when timing is enabled. |
| `total_io_time_ms` | Accumulated disk cache IO time. |
| disk-cache diagnostic snapshot | Most recent disk-cache load diagnostic, including skipped/miss/hit/error status and error details when a read fails. A private diagnostic store exclusively owns both the optional value and one no-throw mutex; record, snapshot, clear/reload reset, compute clone, and staged publication all cross that store, and readers receive independent value snapshots. |

External code must not mutate graph structure through raw node-map access.
Reads use helpers such as `node()`, `find_node()`, `node_ids()`, and controlled
iteration. Structural changes use helpers such as `add_node()`,
`replace_node()`, `remove_node()`, and input-rewire APIs, which validate and
refresh topology adjacency before returning. Runtime cache/state updates may
use `mutable_node()` for node-local runtime fields, but structural edits still
belong to the model mutation helpers.

Internal services coordinate locking, timing, cache, topology, and traversal
behavior through the model boundary. Frontend, CLI, and TUI code reaches graph
state through the public `ps::Host` seam. The embedded Host adapter delegates
to the internal `InteractionService`/`Kernel` boundary; backend services may
use that internal boundary but do not expose it to frontend callers.

For CLI-loaded graphs, `cache_root` is derived from the loaded
`cache_root_dir` config as `<cache_root_dir>/<graph_name>`, with relative paths
resolved from the process working directory. Lower-level `Kernel::load_graph`
callers that omit a cache root keep the session-local fallback
`<root_dir>/<graph_name>/cache`.

`GraphModel::clear()` resets model-level runtime state, not only nodes.
Clearing a graph resets nodes, topology adjacency, timing results,
accumulated IO time, skip-save state, and other per-run state so reload behavior
is not polluted by stale metadata. Disk-cache diagnostic reset uses the same
encapsulated store as worker record and reader snapshot; no GraphModel method
can directly read, write, copy, swap, or reset its optional/path/string storage.

## GraphDefinition and the In-Memory Adapter

`GraphDefinition` is a deep-owned, format-neutral value for one complete graph
document. It owns an ordered vector of `NodeDefinition` values. Each
`NodeDefinition` contains only persistent identity, operation type/subtype,
image and parameter edges, static `ParameterMap`, output and cache descriptors,
and the `preserved` flag. It has no runtime parameters, computed outputs,
revisions, ROI/LUT state, timing, dirty state, or cache results.

`InMemoryGraphDocumentAdapter` is the only translator between that detached
value and private `Node`/`GraphModel` state:

- apply validates duplicate ids and required parameter-edge names while
  staging a complete `GraphModel::NodeMap`, then calls
  `GraphModel::replace_nodes()` exactly once;
- capture visits graph nodes in ascending id order and copies only persistent
  fields into an independent definition;
- single-node materialization/capture supports the ABI-stable
  `Host::get_node_yaml()` / `Host::set_node_yaml()` operations without
  restoring YAML methods on `Node`.

The adapter owns no graph, file, parser tree, cache, or thread. Callers retain
the existing `GraphStateExecutor` serialization responsibility. A definition
or topology failure before replacement preserves the prior node map, topology,
generation, and runtime state.

`GraphDocumentReader` and `GraphDocumentWriter` are separate format-neutral
contracts. Complete-graph methods exchange filesystem paths and detached
`GraphDefinition` values; node methods exchange owned text and detached
`NodeDefinition` values. Neither contract exposes yaml-cpp, `GraphModel`,
`Node`, cache state, or provider-library types.

`GraphIOService` requires non-null shared reader/writer owners. It retains
model orchestration only: load asks the reader for a detached definition and
applies it through `InMemoryGraphDocumentAdapter`; save captures a definition
before calling the writer; node-document operations cross the same injected
boundary. It constructs no parser, emitter, or graph-document stream.

The configured `YamlGraphDocumentAdapter` owns the private YAML translator,
filesystem read, node-text conversion, complete emission, and direct
open/write/flush/close behavior. `create_embedded_host()` selects that adapter
when YAML is enabled and an explicit unavailable document adapter when it is
disabled, then injects the same shared owner as both contracts through
`Kernel`; Kernel and GraphIO have no default persistence construction. A
private explicit-dependency Host root supports deterministic fake substitution
without adding an installed API. Issues #61 and #62 establish the neutral
document/value boundary. Issue #63 completes the dependency-disabled product
profile and preserves empty/in-memory sessions while explicit document IO
returns `GraphErrc::Io`.

## Topology Adjacency

`GraphModel` owns `GraphTopologyIndex`, which records both directions of graph
edges:

- `incoming_by_node`: upstream dependencies for a node.
- `outgoing_by_node`: downstream dependents for a node.

Each `GraphTopologyEdge` stores stable source and target node ids, edge kind
(`ImageInput` or `ParameterInput`), source output name, target input/parameter
identity, and input slot index. Successful graph load, clear, node addition,
node replacement, node removal, and input rewiring refresh or clear this index
before graph state is visible to traversal, compute, cache, inspection, CLI, or
interaction consumers.

## Node Identity

Each `Node` has:

| Field | Meaning |
| --- | --- |
| `id` | Unique integer id inside the graph. |
| `name` | Human-readable label. |
| `type` | Operation family, such as `image_process`. |
| `subtype` | Operation subtype, such as `gaussian_blur`. |
| `preserved` | Prevents some force-recompute paths from discarding the node. |

Operation lookup uses `type:subtype` through `OpRegistry`.

## Inputs

Node inputs are split by data kind:

| Input type | Structure | Meaning |
| --- | --- | --- |
| Image input | `ImageInput` | Reads an upstream image-like `NodeOutput`. |
| Parameter input | `ParameterInput` | Reads an upstream named data output and writes it into runtime parameters. |

## Parameters

`NodeDefinition::parameters` and `Node::parameters` are
`plugin::ParameterMap` values containing deep-owned static data. The configured
YAML adapter's private translator converts a graph document into a detached
definition once; the in-memory adapter then copies that definition into Graph
state. Neither the definition nor Graph storage retains the source YAML tree.
Values use the exact `ParameterValue` alternatives `Null`, `Bool`, `Int64`,
`Double`, `String`, `Array`, and string-keyed `Object`.

Inspection renders these values through the format-neutral
`format_parameter_value_for_inspection()` helper. Scalar spellings remain
stable, arrays and objects are rendered recursively, object keys keep their
ordered-map order, and strings are quoted and escaped without constructing a
YAML node or emitter.

`Node::runtime_parameters` is another `ParameterMap`, rebuilt for execution by
copying static values and applying `parameter_inputs`. Connected named outputs
replace same-name static values without a format conversion. Operators should
read effective values from `runtime_parameters` during compute. Executors
populate it on request-local node snapshots; it is not committed as reusable
Graph state.

## Outputs

`NodeOutput` contains:

| Field | Meaning |
| --- | --- |
| `compatibility_image` | Inbound-only staging for codecs and remaining Host/legacy adapters. It must be cleared before formal commit and is never cache, allocation, readiness, or revision authority; operation ABI v1 uses complete Value/grant records. |
| `named_values` | Canonically ordered immutable Values. The current image port is permanently named `image`; every image or generic entry is the sole payload, allocation, readiness, and revision authority for that exact name. |
| `data` | Named parameter-result scalars or structures stored as a `plugin::ParameterMap`; generic Values never enter this field. |
| `space` | Spatial transform, scale, and ROI metadata. |
| `debug` | Worker/device/timing/range diagnostics. Enabled CPU range inspection walks active scalar bytes through the canonical Value layout; padding is excluded and opaque device Values retain provider diagnostics. |

Operators may return a canonical image Value, independently named generic
Values, parameter results, or an authorized combination of those categories.

Return shape is not authorization. Issue #130 freezes a
`PlannedOutputAuthority` from the selected registered operation revision before
execution: it carries the exact canonical-image requirement, exact named-data
sets for generic Values and parameter results, implementation/device identity,
required canonical-image DenseTensor/ImageFacet/Strided structure, and any
trusted finite Graph or dirty extent. Each generic Value must retain valid
revision/producer identity and valid nonempty indexed storage bindings; its
representation/layout is either DenseTensor with Strided or Blocked layout, or
ProviderDefined with ProviderDefined layout. Generic Values do not acquire an
image-facet requirement. A normal result must match every category exactly;
missing, extra, malformed, wrong-name, wrong-facet, wrong-layout,
wrong-identity, or wrong-extent output is rejected before dependent release or
the first formal Graph mutation. Supervised staging may retain exact Pending
Values, but every formal boundary requires all declared Values to be Ready.
Full HP routes stage all computation in a Graph clone and publish the complete
snapshot only after authorization, so an empty `NodeOutput` can never
manufacture Whole validity or a cacheable completion.

Persistent `OutputPort::output_parameters` is an optional deep-owned
`ParameterValue`. An empty optional means the document field was absent; an
engaged null value preserves an explicitly present YAML null. Nested output
configuration therefore survives parser destruction without retaining
`YAML::Node`.

For tiled `image_mixing`, a secondary input that requires crop/pad is
materialized as a request-local `NodeOutput`: named data, spatial/debug
provenance, and plugin-library lifetime are copied, while its image Value is
replaced by aligned storage produced through kernel fill/copy primitives and
sealed before the normalized output is exposed. “Named data” here includes
generic named Values and parameter results without moving either category into
the other.
Resize and channel conversion remain local OpenCV algorithm calls. The
normalization context owns these temporary outputs until every synchronous tile
callback finishes; exact-shape inputs continue to borrow the upstream output.

DI-2 freezes `DenseImageOutputPlan` as the one source-private ordinary-image
output description. The immutable plan owns the output name, complete
DenseTensor/ImageFacet facts, exact positive Strided layout, byte envelope,
base alignment, and full image Region before Host allocation. One
`HostOutputBinding` owns the aligned allocation and private builder lease.
Move-only whole or disjoint-tile grants expose only checked row spans; overlap,
range, alignment, overflow, cancellation, exception, duplicate retirement, or
omitted retirement fails the binding closed. Only after every grant retires
successfully may the binding seal and publish one Ready Value exactly once.
The plan is the sole internal DI-3 mapping source, not an interim ABI record.

## Cache Fields

The cache-related node fields are:

| Field | Status | Meaning |
| --- | --- | --- |
| `cached_output_high_precision` | Formal cache | HP cache for full-quality reusable output. |
| `hp_version` | Formal cache metadata | Monotonic revision of the reusable HP output. |
| `hp_region` | Formal cache metadata | Normalized logical Region known valid in that HP output. |

Only HP output is formal reusable cache. That means only HP output may feed
subsequent HP compute, configured disk-cache persistence, or a separately
requested output operation; the cache entry itself is not long-term
user-output authority. RT output is not stored on `Node`; it lives in
`RealtimeProxyGraph`, which mirrors node ids and stores low-resolution proxy
output, HP-space Region, version, and RT dirty-source generation.

Dirty RT worker tasks stage proxy output through `RealtimeProxyWriteBuffer`
before committing to `RealtimeProxyGraph`. Dirty HP worker tasks stage formal
HP output through `HighPrecisionDirtyWriteBuffer` before committing to
`GraphModel`, with RealTimeUpdate HP commits gated behind successful RT proxy
commit.

## YAML Schema

Graph YAML root is a sequence of node objects. Supported node fields:

```yaml
- id: 1
  name: source
  type: image_source
  subtype: path
  preserved: false
  image_inputs:
    - from_node_id: 0
      from_output_name: image
  parameter_inputs:
    - from_node_id: 2
      from_output_name: value
      to_parameter_name: strength
  parameters:
    path: assets/input.png
  outputs:
    - output_id: 0
      output_type: image
      output_parameters:
        color_space: linear
        channels: [red, green, blue]
  caches:
    - cache_type: image
      location: output.png
```

`id` is required. Other fields use the configured YAML adapter translator's
established defaults. `parameter_inputs` require non-empty
`from_output_name` and `to_parameter_name`. `output_parameters` may be absent,
explicitly null, or any representable recursive `ParameterValue`.

## Spatial Metadata

`SpatialContext` carries transform and ROI metadata used by ROI propagation and
inspection:

| Field | Meaning |
| --- | --- |
| `transform_matrix` | Global transform matrix. |
| `inverse_matrix` | Global inverse transform. |
| `local_inverse_matrix` | Local inverse used for upstream ROI projection. |
| `absolute_roi` | Output extent or valid region. |
| `global_scale_x`, `global_scale_y` | Scale metadata. |

`SpatialDependencyMap` is an optional node-local LUT for data-dependent spatial
propagation.

## Boundaries and Rationale

- `GraphDefinition` is a detached private document value; `GraphModel` and
  `Node` are private backend runtime state. Public Host callers and operation
  plugins receive copied public values rather than model references.
- Structural mutation goes through model helpers so node storage, both
  adjacency directions, topology generation, and cached planning state become
  visible as one coherent graph state.
- Schedulers receive ready-task metadata and never own node storage,
  parameters, output values, topology, or cache authority.
- `YAML::Node` remains only inside private YAML adapters for graph documents,
  shared value translation, and configured cache metadata. It is not declared
  by runtime, graph, compute, inspection, or cache contracts and is not owned
  by `GraphDefinition`, persistent `Node` fields, or `OutputPort`.
  Static/effective parameters, output-port configuration, and named operation
  outputs are `ParameterValue` trees. Logical dirty work and cache validity use
  normalized `RegionSet`; current image extents, physical tiles, Host/IPC v2
  inspection, and operation ABI v1 adapters use checked derived `PixelSize` and
  `PixelRect` values. In compute/dirty compatibility paths those rectangles are
  zero-based storage coordinates relative to the owning data window; they are
  never retained as logical metadata. Conversion to or from a signed logical
  `ImageRect` requires checked origin translation through that exact
  `ImageBounds`. OpenCV geometry is created only inside an OpenCV provider or
  algorithm implementation when a matrix slice or library call requires it.

### Current persistence identity and completion boundaries

The current Graph document serializes authored node topology/configuration
only. It excludes `NodeOutput`, formal HP cache bytes, RT proxy state,
allocation/value identities, producer fences, daemon job ids, delivery leases,
and output-store records. A successful `GraphIOService::save()` captures a
detached definition and completes the configured YAML adapter's direct
open/write/flush/close sequence; the document has no persisted version and the
call returns no atomic-replacement or durability receipt.

Configured disk-cache locations remain backend cache identities, not Graph
document ids or user-output commit ids. `ImageArtifactCodec` and
`CacheMetadataCodec` convert image and metadata representations but do not own
transaction, retry, visibility, or durability policy. The private IPC
`OutputStore` keeps a separate process-scoped delivery record and artifact
identity; those values are not fields of `GraphModel`, `NodeOutput`, or the
Graph document.

`ReadyFence::Ready`, formal HP publication, disk-cache save, Graph-document
save, daemon result availability, and protected artifact publication are
therefore different current facts. The accepted target authority and
completion taxonomy are recorded in
[ADR 0009](../adr/0009-compute-io-durability-and-completion-semantics.md);
they are not additional current fields.

### Implemented V-3 ownership through V-15 extension surfaces

[ADR 0008](../adr/0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md)
accepts the complete generic-value replacement. V-2 introduced the bounded CPU
DenseTensor subset; V-3 now connects its physical ownership and formal HP cache
identity:

- installed `DenseTensorDescriptor` keeps concrete shape,
  `ElementSemantics`, `StorageEncoding`, and optional quantization separate;
- installed ordinary `ImageFacet` assigns distinct x/y and optional channel
  axes, requires a signed half-open data window, and may retain an independent
  display window, stable channel/group schema, declared sample-domain facet,
  and color facet;
- `BufferHandle` is a checked immutable nonempty range over one explicit
  storage binding, exposes no raw or native pointer, and creates checked
  identity-preserving subranges; CPU builders own host bytes, while
  source-private device publication may retain an opaque native owner and
  independently record host visibility;
- `ValueBuilder` owns the only move-only `WriteLease`, refuses seal while a
  lease is live, and publishes immutable bytes with a fresh `ValueRevisionId`;
- the vector convenience constructor still deep-copies lvalue and rvalue
  descriptor/layout/payload allocations before seal;
- `StridedLayout::byte_offset` anchors logical coordinate zero; immutable
  Values over sealed handles may use bounded positive, zero, or negative
  strides;
- `DenseTensorView` and `ImageView` retain both the Value and a `ReadLease`, use
  copy-like move semantics, and expose pointers only within that lease; and
- `image_process:invert_dense` performs descriptor-only inference and
  stride-aware unsigned-8 execution, reuses a sealed input Value when present,
  and publishes its exact sealed result revision.

Private `NodeOutput::named_values` is the only formal Value authority; the
`image` entry replaces the former image-buffer/value pair. A producer-pending
Value may live in request-local temporary output only: `TaskSubmissionPlan`
holds its Run unsettled and releases dependants after terminal Ready, while
Failed, ProducerCancelled, or stale-typed completion releases none. Ordinary
HP commit, sequential HP compute, connected-preflight shadow cache, dirty HP
commit, and disk decode reject or normalize compatibility staging before
formal publication. Immutable cache copies preserve allocation and revision;
dirty/tiled execution creates one fresh Host binding and seals once after all
selected executable grants retire; replacement and disk decode mint fresh
identities. Allocation and revision tokens remain process-local and never enter
task-graph keys, cache paths, graph/YAML documents, or artifact bytes.
The shared operation runtime is the one process-wide minting authority for the
static Host and every Value-using DSO.

Dirty HP/RT uses the same rule for a source-private Pending Value.
`DirtyReadyTaskContext` queues a Run-scoped continuation without blocking a
worker, retains the exact revision/allocation/producer/staged-Value identity,
and releases dependants only after that same Value becomes Ready and passes the
frozen plan again. Failure, producer cancellation, Run cancellation,
supersession, or staged-value replacement publishes no formal output.
Cancellation callbacks do not decrement worker-owned logical task accounting;
prepared source/dependent contexts remain retained until their matching service
callbacks have settled.

V-4 installs `RegionDomainKey`, `ImageRect`, rank-general `TensorSlice`,
`RegionAtom`, immutable normalized `RegionSet`, bounded algebra, typed
operation outcomes, and containment. `Node::hp_region` is validity metadata
published with the one formal HP cache authority. Dirty source history,
per-node state, monolithic work, and edge mappings retain Region; image-only
tile rectangles are derived beside their source Region. The core dense invert
  path executes exact ImageRect or TensorSlice selections, while RT rejects
  TensorSlice. Operation ABI v1 carries the bounded matching Region records.
  Exact one-clause
difference preserves every equal constrained-domain atom when only one
compatible atom varies and the overlap removes one of its edges; differences
that would split an atom or vary multiple domains remain typed `TooComplex`.

Issue #129 / DI-1 now makes the ordinary built-in DenseImage metadata concrete.
The required signed half-open `ImageBounds` data window is the immutable logical
pixel domain; its x/y spans must exactly match the descriptor shape at the
explicit axes. Negative and nonzero origins are valid. The optional display
window is presentation metadata, while dynamic dirty/dependency/execution/HP
validity remains `RegionSet`. `Value::image_bounds()` exposes data-window
metadata in Pending, Failed, and ProducerCancelled states without weakening
Ready-only payload leases; `ImageView` separately exposes zero-based storage
indices and signed logical-coordinate access. Dirty planner entries therefore
retain both an exact data window and a storage-relative `PixelRect`; their
`RegionSet` validity is reconstructed only by checked origin addition.

The optional bounded `ChannelSchema` uses stable nonzero `ChannelId` and
`ChannelGroupId` values; diagnostic names do not select roles or enter semantic
equality/digests. Version-1 `SampleEncoding`/`SampleDomainFacet` declares
normalized, legal, or code-value intervals and stable-ID per-channel overrides.
Version-1 `ColorFacet` binds a valid channel group to explicit transfer function
and primaries. Storage-representable range remains a property only of element
semantics and storage encoding; quantization, declared sample meaning, and color
remain independent. Observed min/max/histogram queries, results, and complete
revision/content/Region/selector/algorithm cache keys are independent derived
values and never become Value or descriptor/content identity.

V-6 attaches an installed, copyable `ReadyFence` observer to every Value.
Synchronous publications start Ready. A source-private pending producer keeps
the only mutable CPU allocation capability, revokes it before Ready, Failed, or
ProducerCancelled, and leaves descriptor/layout/size/identity metadata
inspectable while payload access is rejected. The source-private physical
`ValueTransferTask` allocates a distinct pending CPU destination and copies its
validated envelope only from executor-queued work after source readiness.

V-8 installs `DeviceBackend`, checked `DeviceId`, `MemoryDomain`,
`StorageBinding`, producer identity, and exhaustive `AccessPlan` outcomes.
Planning is nonblocking and records source facts, exact target capability,
visibility obligations, lease kind, and transfer bytes without touching
payload. Host access still requires both producer Ready and a host-visible
binding; otherwise it throws without an implicit wait, map, import, transfer,
or readback. Explicit CPU/Metal transfers publish distinct bindings while
preserving one logical `ValueRevisionId`. The process `ResidencyManager` indexes
only exact Ready replicas and atomically gates destination readiness on complete
completion identity plus current supersession generation. A fallible
prepublication step creates the lineage row without assigning a managed current
identity. Accepted coordinator publication assigns the exact generation,
including a coordinate-authorized numeric decrease, before exposing
currentness. A later stale Run observation or transfer admission cannot replace
that exact managed identity; standalone lineages separately retain
numeric-maximum ordering. Settled replicas may
outlive their producing Run, but strong native/provider ownership is bounded by
the manager's 64-entry default: publication pressure releases the
lowest-revision entry. A managed-current assignment alone does not clear residency, and
the entry count is not device-byte or scratch admission. After exact Graph
close has drained every Run and pending native completion, the manager retires
all generation rows for that nonreused `GraphInstanceId`. The close tail also
joins the compute-request lane before this retirement: a prepared candidate
already owns one reserved lane ticket before its fallible lineage pretracking,
so no caller can recreate a zero-generation row afterward. Calling retirement
while a transfer remains pending is an invariant failure. This Graph-scoped
maintenance does not clear settled resident replicas.

V-9 adds byte authority without changing logical Value identity or public
binding facts. `ResourceLedger` owns an isolated memory/scratch account for
each configured non-CPU `DeviceId`. Native plans use backend size/alignment
facts; actual allocations use `allocatedSize`. A persistent device `Value`'s
type-erased external owner holds the unique memory lease beside its native
allocation, so Value copies and residency retain—not duplicate—the authority.
Scratch remains outside the Value and follows the exact asynchronous
completion owner. A completed HostPinned readback retains its shared Metal
buffer as CPU-owned output storage after the scratch lease ends.

V-12 verifies this installed model across the dimensions most likely to expose
image-only assumptions. The dependency-neutral matrix covers 1/3/4/8/16
channels and FP32/FP64 for padded image-faceted Values, rank-one through
rank-five FP32/FP64 latent Values, exact ImageRect/TensorSlice merge, and
bounded negative- and zero-stride immutable views. The rank-one fixture has a
sole stride wider than its element, an exact padded storage span, and an
independent byte oracle for active elements and padding sentinels. CPU-copy and
injected external-device preparation share the builder's positive,
zero-offset, exact-envelope, non-overlap validation authority. Negative or
zero strides therefore fail before an external owner, destination identities,
or a Pending fence can escape, while the general immutable publisher retains
its signed-view role. Supported transfers preserve the complete positive
producer envelope, descriptor, facet, layout, and logical revision while
minting a distinct allocation and exposing Pending-to-Ready binding facts. An
admitted `ComputeIoExecutor` task retains and observes the same immutable Value
facts and bytes under explicit task/byte budgets; that observation creates no
cache, artifact, or persistence identity.

V-13 installs one bounded packed/quantized execution vertical without
reinterpreting the byte-addressed model. `StorageEncodingKind::Fp4E2M1`
identifies four-bit E2M1 independently from floating-point semantics, while an
optional `QuantizationSchema` owns a rank-matched positive block shape and one
finite positive scale per row-major logical block. The shape must divide into
complete blocks. A version-1 `BlockedLayout` separately records the matching
block shape, nibble-aligned block bit strides, absolute bit offset, and explicit
least- or most-significant-first nibble order. Publication proves exact byte
bounds and non-overlapping complete block spans. `Value` retains exactly one
tagged Strided or Blocked layout; `dense_tensor_element_bytes()` and
`DenseTensorView` reject packed storage, while `PackedDenseTensorView` provides
checked encoded-code and scale-dequantized access without manufacturing an
element byte pointer.

The installed packed execution operation accepts only a matching-domain,
full-rank, nonempty TensorSlice whose endpoints align to every quantization
block. It directly copies FP4 codes and selected scales into a fresh CPU Value,
preserves nibble order and bit offset, and emits canonical contiguous block bit
strides without dequantization or requantization. Explicit CPU and injected
external-device transfers preserve the complete descriptor, quantization,
Blocked layout, byte envelope, unused nibble bits, readiness transition, and
logical revision in a distinct binding. Formal HP memory cache copies retain
that immutable Value and exact TensorSlice validity. The current image-only
disk cache instead rejects packed, quantized, or latent formal Values with
`GraphError{InvalidParameter}` before compute-I/O admission, filesystem
mutation, or codec invocation; no generic artifact format or persistent digest
is implied.

V-14 adds one explicit `ProviderDefined` representation beside DenseTensor.
`DataDescriptorEnvelope` retains exactly one versioned Schema record plus
bounded ordered Facet records; `ProviderDefinedLayout` retains one versioned
Layout record plus checked `{buffer_index, logical_role, offset, length}`
envelopes. Every extension record owns its unknown payload bytes. A
provider-defined `Value` retains multiple sealed host-readable
`BufferHandle`s, an immutable exact provider generation, and one fresh
process-local revision. Generic cross-reference and checked-end validation
run before provider validation and before publication identities are minted.
DenseTensor-only accessors reject this representation; indexed
`ProviderReadLease` keeps both the selected buffer and interpretation
generation live.

`DataDefinitionRegistry` is one injected, non-singleton authority with one
generation source, one provider table, and separate typed Schema, Facet, and
Layout maps under one publication lock. Candidate loading stages and validates
the complete exact-size v3 definition bundle before one atomic publication;
typed-key conflicts or malformed records preserve all visible prior state.
Replacement publishes a fresh complete generation. Unload removes only new
lookup visibility: old Values, reads, callbacks, and provider-created owners
retain the retiring generation until final provider destruction, after which
the candidate module lease releases. No callback runs while the registry lock
is held.

The v3 provider ABI implemented by this slice is the dependency-neutral
definition suite only. Its self-contained C11/C++17 header freezes exact record
sizes, offsets, alignment, calling convention, statuses, two exported
handshakes, and mandatory validation, pure property, pure Region, pure
DataSpec, canonical-content, owner, and destroy callbacks. Pure callbacks
receive descriptor/Layout/buffer metadata with every payload pointer cleared;
validation and canonical-content traversal are the only semantic callbacks
that receive payload in V-14. Access, mapping, transfer, conversion, inference,
execution, native-device, and operation-plugin authority are absent.

Borrowed ABI byte views are input-only. Each callback receives one Host-owned
output sink; diagnostic and BYTES-property records declare scalar lengths, and
providers synchronously copy complete fields while callback-local source
storage is still alive. Per-invocation Host state enforces exact channel use,
4 KiB/64 KiB bounds, and sticky failure without sharing storage across threads
or generations. For Exact nonempty TensorSlice results, the Host independently
checks that `selected_site_count` equals the checked product of every half-open
axis length; overflow or mismatch becomes InvalidDescriptor with zero sites.

The bounded V-14 `DataSpec` evaluates Schema identity/version and logical-site
ranges into Subset, Disjoint, PartialOverlapWithRuntimeGuard, or
CannotEvaluate. Property and Region calls preserve their typed unavailable or
uncertain outcomes, including Empty/Unsupported Region states around exact
TensorSlice count validation. Descriptor, storage-layout, and provider-selected
logical content use separately tagged SHA-256 canonical traversals; physical
buffer order, offsets, and padding do not enter ContentDigest when the provider
emits the same logical byte stream. To preserve the frozen length-prefixed
field without staging that stream, the Host first performs a checked
`uint64_t` measurement traversal, then repeats the same active generation
under the same immutable Value view and payload read leases while feeding
SHA-256 incrementally. The two invocations use independent callback-local
diagnostic state. Chunk boundaries are irrelevant; malformed pointer/count
pairs, overflow, sticky sink failures, and measured/hash count drift are typed
provider failures. There is no payload-proportional staging or arbitrary
64 MiB content ceiling. The versioned artifact envelope preserves
Schema/Facet/Layout unknown bytes and all three optional digest identities
without a provider, but it is not a graph document, manifest/chunk store,
filesystem codec, or cache-policy integration.

The same installed `compute_content_digest(Value)` entry gives built-in
DenseTensor values a frozen canonical-v1 stream identity without invoking a
provider callback. The built-in Schema structural version 2 encodes rank,
shape, element semantics, storage encoding kind and width, and optional
quantization block shape plus binary32 scale bits. Image structural version 2
encodes axes, signed data/display windows, stable channel order, group IDs, and
members; independent Sample Domain and Color Facets use structural version 1.
Diagnostic names and observed statistics are absent. Content traversal follows
row-major logical coordinates: whole-byte scalars are emitted little-endian,
while blocked FP4 emits one low-nibble code byte per logical element. Strides,
byte/bit offsets, padding, block placement, nibble order, allocation/binding
identity, device identity, readiness metadata, and Value revision do not enter
logical content identity. Descriptor-bound quantization metadata does enter the
descriptor digest. A non-Ready or unreadable payload returns
`PayloadUnavailable`; malformed or unsupported retained state returns
`InvalidDescriptor`; allocation failure still propagates as `std::bad_alloc`.
These rules make repacked but logically equal DenseTensor outputs comparable by
typed `Sha256CanonicalV1` `ContentDigest` rather than raw storage bytes.

V-15 supplies the first optional concrete generation for that unchanged v3
definition suite. The repository OpenEXR provider publishes one
`VariableSampleField` Schema, `ImageFacet`, `DeepSampleFacet`, and one
multi-buffer Layout. Its versioned descriptor payload preserves signed
half-open data/display windows and an explicit mapping from diagnostic file
channel names to permanent channel identities, semantic-role identities, and
Layout buffer roles. Names carry no inferred semantics. Its canonical Value
stores one little-endian `uint32` sample-count buffer, one checked
little-endian `uint64` prefix-offset buffer, and one identity-ordered FP32
sample stream per unit-sampled channel. Every stream agrees on the same
declared sample count. Because the unchanged V-14 binding contract requires
every published semantic buffer envelope to have nonzero length, an all-zero
deep image retains its channel mapping in descriptor/Layout payloads but omits
the zero-length channel envelopes and BufferHandles. Its nonempty count and
offset buffers remain the complete canonical content traversal.

The source-private OpenEXR adapter decodes through an injected
`DataDefinitionRegistry` and `Value::from_provider_defined`; it therefore
reuses V-14 cross-reference validation, generation retention, indexed read
leases, Region/DataSpec/properties, and all three generic digests. Encoding
inspects that generic Value rather than defining a second deep-image object
model. The first format is strictly complete single-part deep scanline;
deep-tiled, multipart, shallow or mixed-part files, absent/malformed explicit
mapping, non-FP32 channels, and non-unit sampling fail with Host-owned typed
errors.

Additional packed encodings or quantization formulae, unaligned requantizing
slices, general Map/Import providers, the remaining provider ABI suites,
generic graph/cache persistence, and general named immutable Value outputs
remain later no-shim slices. V-14 does not add public resource declarations,
general heap suballocation, device-queue budgets, or manifests/chunks. V-15
does not add deep-tiled or multipart support, a graph/compute execution path
for provider-defined Values, generic graph/cache persistence, or public
OpenEXR types. `PHOTOSPIDER_BUILD_OPENEXR_DEEP_PROVIDER` defaults OFF; in that
profile no OpenEXR header, link, symbol, package lookup, or transitive
dependency enters the dependency-neutral product.
`ParameterMap` remains configuration and current named scalar-result storage.

Keeping graph identity and topology in one model makes traversal, compute,
inspection, and mutation observe the same generation. Issue #62 completes the
runtime/cache YAML value boundary without making the configured product
dependency-optional. The remaining configured-product and provider-library
dependency work is governed by
[ADR 0002](../adr/0002-external-libraries-are-kernel-adapters.md) and the exact
[dependency-neutral kernel target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel);
neither document changes the current fields described above.

## Implementation and Validation Entry Points

- `include/photospider/data/value.hpp`
- `include/photospider/data/extension.hpp`
- `include/photospider/data/image_view.hpp`
- `include/photospider/data/packed_dense_tensor_view.hpp`
- `include/photospider/data/region.hpp`
- `include/photospider/core/device.hpp`
- `include/photospider/memory/access_plan.hpp`
- `include/photospider/memory/blocked_layout.hpp`
- `include/photospider/memory/buffer_handle.hpp`
- `include/photospider/memory/ready_fence.hpp`
- `include/photospider/memory/strided_layout.hpp`
- `include/photospider/plugin/data_definition_registry.hpp`
- `include/photospider/plugin/data_provider_api.h`
- `src/lib/graph/graph_model.*`
- `src/lib/graph/node.hpp`
- `src/lib/graph/graph_definition.hpp`
- `src/lib/graph/graph_document_reader.hpp`
- `src/lib/graph/graph_document_writer.hpp`
- `src/lib/graph/in_memory_graph_document_adapter.*`
- `src/lib/adapters/yaml/graph_definition_yaml.*`
- `src/lib/adapters/yaml/yaml_graph_document_adapter.*`
- `src/lib/adapters/yaml/parameter_value_yaml.*`
- `src/lib/adapters/yaml/yaml_cache_metadata_codec.*`
- `src/lib/core/cache_metadata_codec.hpp`
- `src/lib/ipc/output_store.*`
- `src/lib/core/pending_value.hpp`
- `src/lib/core/value.cpp`
- `src/lib/core/dense_tensor_content_digest.*`
- `src/lib/core/extension.cpp`
- `src/lib/core/packed_dense_tensor.cpp`
- `src/lib/core/value_image_adapter.*`
- `src/lib/core/region.*`
- `src/lib/core/region_image_adapter.*`
- `src/lib/core/cpu_dense_image_operation.*`
- `src/lib/core/ops.cpp`
- `src/lib/core/parameter_value_text.*`
- `src/lib/execution/transfer/value_transfer_task.*`
- `src/lib/execution/device/device_completion.*`
- `src/lib/execution/device/residency_manager.*`
- `src/lib/plugin/data_definition_registry.cpp`
- `src/lib/adapters/openexr/openexr_deep_contract.hpp`
- `src/lib/adapters/openexr/openexr_deep_scanline_adapter.*`
- `plugins/data/openexr_deep_scanline_provider.cpp`
- `src/lib/graph/graph_io_service.*`
- `src/lib/core/ps_types.*`
- `src/lib/compute/dirty/tiled_input_normalizer.*`
- `src/lib/compute/request/compute_metrics_recorder.*`
- `tests/unit/test_graph_topology_boundaries.cpp`
- `tests/unit/test_graph_document_adapter.cpp`
- `tests/integration/test_graph_document_injection.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_stride_aware_compute_paths.cpp`
- `tests/unit/test_dense_tensor_content_digest.cpp`
- `tests/integration/test_graph_document_errors.cpp`
- `tests/integration/test_cpu_dense_tensor_image_operation.cpp`
- `tests/integration/test_packed_fp4_dense_tensor.cpp`
- `tests/integration/test_variable_sample_field_extensions.cpp`
- `tests/integration/test_openexr_deep_scanline_provider.cpp`
- `tests/integration/openexr_deep_provider_option_off_smoke.py`
- `tests/unit/test_region_contracts.cpp`
- `tests/integration/test_value_identity_dso.cpp`
