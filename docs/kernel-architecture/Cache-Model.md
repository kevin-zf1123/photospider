# Kernel Cache Model

The kernel has formal HP memory cache on each `Node`, transient RT proxy state
in `RealtimeProxyGraph`, and disk cache files under the graph cache root. This
document defines the current cache semantics.

## Formal Cache and Transient State

| Location | Status | Meaning |
| --- | --- | --- |
| `Node::cached_output_high_precision` | Formal cache | Full-quality HP output owner; whole-output reuse also requires complete `hp_region`. |
| `RealtimeProxyGraph` node state | Transient RT proxy | Low-resolution interactive preview/update output. |

Only high-precision output is formal reusable cache. That means only HP output
may be used as the authoritative source for subsequent HP compute, disk cache,
other reusable cache behavior, or a separately requested output transaction.
RT proxy output is transient interactive state and must not be treated as
authoritative cache, as a disk-cache synchronization source, or as direct
output-commit input. Neither formal HP cache nor disk cache is itself the
durable user-output authority.

## HP Cache

HP compute writes `cached_output_high_precision`. HP cache is the authoritative
full-quality result owner for a node. A dirty publication may retain a partial
formal output, but `ComputeCachePolicy` exposes it to whole-output consumers
only when `hp_region` proves complete coverage. For a sealed image-faceted
Value, either the complete compatibility ImageRect or the complete
rank-general TensorSlice is an accepted proof.

Associated fields:

| Field | Meaning |
| --- | --- |
| `hp_version` | Version counter for HP output changes. |
| `hp_region` | Normalized logical Region known valid in the formal HP output. |

## RT State

RT compute writes `RealtimeProxyGraph`. Each proxy node is keyed by the original
graph node id and stores only low-resolution output, HP-space Region metadata,
version, and RT dirty-source generation. It does not copy Node parameters,
inputs, topology, caches, or formal HP state. When the observed graph topology
generation changes, synchronization resets live proxy entries rather than
preserving state by reused node id, so reload/edit workflows cannot expose stale
low-resolution output from an earlier graph.

Dirty RT execution does not write graph-owned RT fields. Worker tasks stage
proxy output, Region metadata, version counters, and dirty-source commit
generation in `RealtimeProxyWriteBuffer`, then commit that staged state to
`RealtimeProxyGraph` after the RT dirty work set drains. Dirty HP execution
similarly stages HP output in `HighPrecisionDirtyWriteBuffer` before committing
to `GraphModel`, so HP/RT siblings can compute concurrently while preserving
RT-first commit ordering.

Associated fields:

| Proxy field | Meaning |
| --- | --- |
| `version` | Version counter for RT proxy output changes. |
| `region_hp` | Normalized signed logical HP ImageRect Region represented by RT update; it is not an RT/storage ROI. |
| `dirty_source_generation` | RT dirty source generation committed for stale source checks. |

## Disk Cache

`GraphCacheService` handles disk cache files under `GraphModel::cache_root`.
Node cache entries describe cache type and location. Each supported configured
location now names one portable named-Value transaction:

| Path | Current role |
| --- | --- |
| configured location | Optional image-codec projection for external inspection; never replay authority. |
| `<location>.yml` | Optional detached `NodeOutput::data` parameter metadata. |
| `<location>.values` | Canonical public `NamedValueArtifactSet` archive containing every formal named Value. |
| `<location>.manifest` | Versioned transaction record written last; it binds archive/metadata counts, byte sizes, generation-derived SHA-256 digests, and one random writer generation. |

The graph-document location is untrusted path input. It must be one nonempty
portable leaf: absolute/rooted or multi-component paths, `.`/`..`, separators,
control or Windows-illegal characters, reserved device basenames, trailing
dot/space, self-aliasing derived siblings, and aliases across configured image
entries are rejected before capture, codec, or filesystem effects. On POSIX,
the cache owner retains no-follow root/node directory descriptors and performs
archive/manifest reads, writes, and controlled deletion through `*at`
operations; every accepted leaf is an owned, regular, single-link file.
Symlink, directory, device, FIFO, hard-link alias, and sparse replay files are
rejected. The current dependency-neutral image/metadata codec interfaces still
accept paths, so their calls are bracketed by directory/leaf identity checks;
this narrows but does not claim to eliminate a malicious same-uid replacement
race inside an external path-only codec.

The archive is the sole persisted Value authority. It preserves exact ordered
names, descriptor and Facet records, layout and binding facts, buffer roles and
envelopes, payload bytes/digests, and applicable descriptor/content/layout
identities. It excludes process-local allocation, Value revision, producer,
fence, mapping, device, and lease identity. In-memory parameter data remains a
detached `plugin::ParameterMap`; `GraphCacheService` never constructs YAML
values.

Every `GraphCacheService` instance resolving the same normalized cache root
shares one weakly retained process coordinator. Save, load, stale cleanup,
synchronization, and drive clear are serialized under that root. Each logical
mutation advances a checked epoch: an admitted asynchronous writer prepared
before a later save, partial cleanup, synchronization, or clear observes that
it was superseded and performs no filesystem work. The weak registry retains
no root after the last operation, same-root callback reentry fails before lock
acquisition, and different roots may proceed independently.

For CLI-loaded graphs, `GraphModel::cache_root` is configured from
`cache_root_dir` before graph load and resolves to
`<cache_root_dir>/<graph_name>`. Relative `cache_root_dir` values are relative
to the process working directory. Direct `Kernel::load_graph` callers that do
not provide a cache root continue to use `<root_dir>/<graph_name>/cache`.

Disk cache image projection precision currently supports `int8` and `int16`
save paths. That explicit conversion affects only the auxiliary image file;
portable replay reconstructs the exact archived Value representation.

The current disk format does not persist Region metadata, so only a complete HP
output may be saved or protect configured disk artifacts during
synchronization. A partial formal output is not loaded over and is never
relabelled as complete. Saving or synchronizing a partial node removes the
older projection, metadata, archive, and manifest instead of encoding partial
bytes. A successful disk load derives complete validity for the freshly
reconstructed output.

Explicit Empty/Whole validity is classified before interpreting the formal
Value. A partial packed, quantized, or provider-incompatible canonical image
therefore runs only controlled predecessor cleanup: it constructs no
`ImageView`, captures no artifact, consults no provider, invokes no codec, and
cannot become a restart hit. Unsupported capability preflight runs only after
complete validity has been established.

An optional ordinary-image projection crosses the private, dependency-neutral
`ImageArtifactCodec` contract.
`Kernel` obtains one configured shared codec from the product composition root
and injects it into `GraphCacheService`; Graph/cache code supplies only paths,
exact ordinary-image Values, and explicit decode/encode sample requests. With
OpenCV enabled, the configured adapter uses OpenCV imgcodecs for non-OpenEXR
formats and translates provider failures to `GraphErrc::Io`, while OpenCV
`StsNoMem` remains `std::bad_alloc`. Its closed write matrix accepts JPEG UINT8
1/3-channel; PNG/TIFF/JPEG2000 UINT8/UINT16 1/3/4-channel; BMP UINT8
1/3-channel; WebP UINT8 3/4-channel; PGM UINT8/UINT16 1-channel; PPM
UINT8/UINT16 3-channel; PNM UINT8/UINT16 1/3-channel; and PAM UINT8
1/3-channel. In particular WebP grayscale, BMP alpha, PBM, PAM alpha, and PAM
UINT16 are rejected before destination mutation. The optional ordinary OpenEXR
codec preserves independent signed data/display windows. With both codecs
disabled, the configured unavailable codec returns `GraphErrc::Io` without
discovering or exporting them. Tests inject a deterministic fake to verify call
order, lifetime retention, precision selection, recoverable errors, and
resource exhaustion without reading or writing a real image format. Real codec
tests encode and decode every allowed OpenCV tuple and verify depth, channels,
and shape.

Detached parameters independently cross the private, dependency-neutral
`CacheMetadataCodec` contract as paths and `ParameterMap` values.
`Kernel` injects and `GraphCacheService` retains this second immutable shared
owner for the same service lifetime. Cache policy still derives the sibling
`.yml` path, creates directories, selects entries, records timing and
diagnostics, preserves HP authority, and removes stale files. The configured
`YamlCacheMetadataCodec` alone owns YAML nodes, recursive value conversion,
stream IO, and provider exception translation. Null documents decode to an
empty map; invalid representations become `GraphErrc::InvalidYaml`, recoverable
write/emission failures become `GraphErrc::Io`, and `std::bad_alloc` propagates
unchanged. A deterministic fake verifies exact paths, values, retained
lifetime, error categories, and resource exhaustion without declaring YAML
types in cache code.

Every load receives a `ValueDiskCacheOutputSchema` copied from the frozen
`PlannedOutputAuthority`: whether the canonical image is planned, the exact
parameter-result names, and the exact generic named-Value names. A transaction
requires both archive and manifest; a retired image/YAML pair or any partial
transaction is an incompatible miss before publication. The reader validates
manifest version/flags/counts, archive byte size and digest, canonical archive
framing, exact planned names, every descriptor/Facet/layout/binding fact and
payload digest, and provider generation before reconstructing a local
candidate. `Kernel` owns one process-domain `DataDefinitionRegistry`, injects
that exact borrowed authority into `GraphCacheService`, and declares it before
the cache service so it outlives every replay. The registry's existing
generation/lease synchronization remains the provider thread-safety and DSO
lifetime boundary. Provider-defined multi-buffer Values therefore replay
through the real embedded/CLI Kernel and GraphRuntime composition; a missing
or incompatible provider is a typed error.

One unpredictable 128-bit writer generation is repeated in every archive
envelope's owner-supplied commit join. Both archive and metadata manifest
records use `SHA256(generation || canonical_byte_size || raw_file_digest)`, so
otherwise valid raw files from different writers cannot form one generation.
When parameter outputs are planned, the manifest also binds the exact metadata
bytes. The reader verifies those bytes before and after codec decode, compares
the exact decoded key set, and finally rereads the manifest. A manifest/payload
race, tamper, mixed generation, stale name, missing file, or one failed Value
publishes none of the candidate. A hit moves the complete `NodeOutput` once and
mints fresh runtime identities for every reconstructed Value. The optional
image projection is never read by replay and therefore cannot become a second
Value authority.

Disk cache load attempts preserve the existing try-load bool contract while also
recording the latest diagnostic through GraphModel's private disk-cache
diagnostic store. That store exclusively owns the optional value and its
no-throw mutex, so worker record, reader snapshot, clear/reload reset, compute
clone, and staged publication cannot bypass one synchronization contract.
Every operation holds the mutex through a private non-copyable scoped guard;
snapshot-copy exceptions release it during stack unwinding, while two-store
publication acquires a `std::less` address order and releases in inverse guard
destruction order. Each store is a direct member of exactly one live or staged
`GraphModel` and owns no worker lifetime. Runtime compute-request work,
graph-state work, and scheduler workers that can reach it must be drained and
joined before that owning model is destroyed; access may not race member
teardown. Callers inspect independent snapshots instead of mutable storage.
The diagnostic result distinguishes skipped attempts, true misses, hits, and
read/parse errors. Invalid manifests, archive/metadata digest mismatch, missing
providers, invalid YAML metadata, and filesystem failures are recorded with an
error code and message instead of being indistinguishable from a normal cache
miss.

## Current Durability and Failure Boundary

Current cache save provides manifest-last publication and all-or-nothing replay,
but it is not a crash-durable or atomic filesystem transaction.
`GraphCacheService` first captures and validates every named Value into detached
archive bytes before planned-byte admission, task construction, filesystem
mutation, or codec invocation. Unsupported Values fail with typed
`InvalidParameter`; `skip_save_cache` and absent/unsupported cache entries keep
their existing no-op policy before that validation.

Construction freezes GraphCache-specific resource limits independently of the
wider public artifact framing ceiling. Current defaults admit at most 512 MiB
for the canonical archive, 16 MiB for detached metadata, 512 MiB for the
auxiliary projection, and 528 MiB for archive-plus-metadata replay. Manifest
facts and checked aggregate arithmetic are validated first; no-follow regular
file type, single-link ownership, physical non-sparse storage, exact size, and
the service limit are checked before archive allocation or digest traversal.
The archive read hashes while filling its sole file-byte owner. That owner is
released after public decode, and artifact payload owners are released one at
a time after reconstruction, bounding overlap instead of retaining an 8 GiB
archive plus duplicate payload sets. `std::bad_alloc` continues to propagate
unchanged.

A complete save writes the optional image projection, optional parameter
metadata, canonical archive, removes excluded projection/metadata predecessors,
captures exact archive/metadata records, verifies metadata codec round-trip,
and writes/readbacks the versioned manifest last. A partial output removes all
four files. A failure may leave a partial or mixed generation, but replay cannot
publish it because the manifest-bound sizes/digests, stable generation, exact
names, and complete artifact set must all validate first. The service does not
provide temporary-file rename, rollback, file/directory synchronization
barriers, a durability receipt, retry protocol, or crash recovery. Sequential
save, parallel committer, compute-I/O executor, cache-all, and synchronization
all converge on this same mechanism.

`cache_all_nodes` counts nodes with present HP output for which the save path
was attempted; the count is not proof that each node had a configured artifact
or that a durable cache entry exists. Cache load diagnostics are process-memory
observations of the latest attempt, not durable audit records.

The current product compute commit policy performs eligible changed-HP cache
writes after exact revision/generation validation but before the no-throw live
Graph swap. It now submits that staged save mechanism to the process-owned
`ComputeIoExecutor` after the live predicates succeed. A passing limit check
provisionally reserves task count and a checked planned-byte estimate before
lazy codec/task payload construction or filesystem side effects. Factory throw,
empty callback, or task/queue-entry allocation failure rolls back without an
Accepted event. Successful construction publishes Accepted either with queue
ownership or, when external shutdown has already won, atomically with its
linked Cancelled settlement before callback entry. The I/O task retains the
prepared Graph transaction, while the graph-state policy owner waits for typed
completion and applies measured I/O time. CPU compute workers cannot perform
that wait. A rejection, cache codec, filesystem, or allocation failure can
therefore still fail that `ComputeRun` and leave live Graph/RT state unchanged.
This ordering is current behavior, not a statement that cache is part of
user-output commit.
[ADR 0009](../adr/0009-compute-io-durability-and-completion-semantics.md)
accepts a different target ordering in which cache persistence has an
independent typed outcome after Run publication.

## Cache Commands

| Operation | Effect |
| --- | --- |
| Clear drive cache | Remove disk cache directory contents and recreate root. |
| Clear memory cache | Clear in-memory HP cache tracked by `GraphModel`. |
| Clear cache | Clear both disk and memory cache. |
| Clear derived image statistics | Remove retained statistics results through the internal cache-service API; accepted in-flight work is not implicitly cancelled. |
| Cache all nodes | Save nodes with complete HP output to disk when configured; partial nodes purge stale configured artifacts. |
| Free transient memory | Clear non-ending node memory cache state. |
| Synchronize disk cache | Save complete HP output and remove stale disk files for nodes without complete validity. |

Disk cache save, load, and synchronization use `cached_output_high_precision`
only. RT proxy output does not protect stale disk files and is not promoted to
disk cache state.

## Boundaries and Rationale

- HP paths write `cached_output_high_precision`.
- RT paths write `RealtimeProxyGraph` as transient interactive state, using
  `RealtimeProxyWriteBuffer` for dirty worker writes before proxy commit.
- Formal cache save/load/sync behavior, subsequent HP compute, and separately
  requested output creation must use HP output and must not promote RT output
  to authoritative cache or durable-output authority.
- Long-lived tests verify HP graph cache and RT proxy graph state
  independently.

`GraphInspectService` selects node-local display metadata from HP cache only.
The current Host inspection surface does not promote RT proxy state into
`GraphModel` or expose it as authoritative cache metadata.

One formal cache authority prevents a low-resolution preview from silently
becoming an HP dependency or persistence source. Request-local staging keeps
partially assembled dirty output invisible until its domain-specific work has
settled.

The current private disk-cache implementation calls neither OpenCV image codecs
nor YAML APIs. It depends on injected `ImageArtifactCodec` and
`CacheMetadataCodec` contracts; configured private adapters own provider
decode/encode, recursive conversion, stream IO, and exception translation.
Issue #62 completes this runtime/cache value boundary. Issue #63 adds
capability-selected real or unavailable adapters: the default product discovers
and links yaml-cpp/OpenCV, while the dependency-disabled product discovers
neither and returns `GraphErrc::Io` for explicit representation IO.
[ADR 0002](../adr/0002-external-libraries-are-kernel-adapters.md) and the exact
[dependency-neutral kernel target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel)
describe the final adapter and document boundary.

## V-3 Runtime Allocation and Revision Identity

A formal HP `NodeOutput` carries canonical ordered named Values. The permanent
`image` entry is the sole image payload/allocation/readiness/revision authority;
every declared generic entry is the corresponding non-image Value authority
and remains distinct from `NodeOutput::data`. Formal cache contains only those
named Value authorities and has no compatibility peer or staging field.
Copying a formal cache entry preserves each Value's revision, producer, representation, indexed
storage bindings, and Ready state; provider-defined multi-buffer Values are not
collapsed to one image allocation identity.

The revisioned generic-name vector participates in planned-route equality,
implementation replacement, and task-graph cache identity. Retained-memory
accounting charges metadata/authority vectors, string payloads, and
`named_values` map nodes. Physical allocation bytes remain charged by their
existing allocation/cache owner and are not counted again as route metadata.

Mutable dirty/tiled work cannot retain or expose the old authority. It creates
one unpublished Host binding, seeds retained bytes through checked grants, and
seals exactly once after every executable grant retires. Replacement output
and disk decode likewise create fresh identities. RT proxy output is a fresh
sealed Value with a separate HP-generation projection version; it remains
transient and does not become a formal cache identity source.

Disk save requires every sealed named Value and captures it through the public
portable artifact boundary. One canonical archive persists ordinary rich images,
generic built-in Values, and compatible provider-defined multi-buffer Values;
the optional image codec projection and detached parameter metadata are not
parallel Value authorities. Neither `AllocationIdentity` nor `ValueRevisionId`
is serialized, reconstructed from a path, or used as a persistent cache/task
key. Both tokens are opaque process-local runtime identities; every replay of
every archived Value necessarily mints new ones.

The configured artifact path remains `cache_root/node_id/location` and contains
no revisioned output-schema component. Consequently, each read carries the
complete frozen image/parameter/generic shape rather than trusting the path.
The versioned sibling manifest and public archive must match that shape exactly;
generic named Values are ordinary archive members, not an incompatible miss.
Old image/YAML-only entries, partial transactions, changed names, tampered
payloads, mixed generations, and missing providers cannot become hits. This
closes schema replacement through one portable transaction without making the
path, auxiliary projection, or detached metadata a second Value identity.

## V-4 Region Validity

`Node::hp_region` is normalized logical validity metadata for the one formal HP
cache authority; it is not another output, allocation identity, Value
revision, disk path, or persistence key. Full compute, sequential publication,
result commit, and successful disk load publish a complete Region (`ImageRect`,
TensorSlice bounds, or Whole when only non-image named data is known).

Dirty HP staging carries output, Region, version, and source generation
together. For the exact core Region bridge, a compatible complete-shaped result
contributes selected bytes only: selected coordinates replace prior bytes and
unselected coordinates remain from the staged output. Existing complete
validity is retained across that merge even when its proof is an ImageRect and
the update is a TensorSlice. Exact representable unions retain accumulated
partial validity. When two partial updates cannot be represented by the bounded
one-clause contract, staging keeps the fresh exact update as a safe
under-approximation rather than publishing a false bounding superset. The
existing revision/current-generation predicate publishes or discards the
entire staged state atomically.

A fresh partial publication remains formal state but cannot satisfy
whole-output reuse or current disk persistence. Normal Whole computation
replaces it and derives complete validity. Generic operation ABI v1 monolithic callbacks
continue to replace complete outputs; selected-byte merging is limited to the
source-private exact core Region implementation.

Every whole-output execution route applies that rule through
`ComputeCachePolicy`, including empty-plan validation, monolithic and tiled
runner cache guards, committed upstream dependency resolution, and final target
return. A dependency-complete current-request temporary result may flow
downstream, but raw presence of partial persistent output never suppresses its
planned recomputation or exposes its bytes to a whole-output consumer.

Dirty planning never interprets exact old output as proof that a Region named
by the current dirty snapshot is already current. The callback-free target cone
is retained after a planning-time cache observation, and every dirty-selected
node remains executable whether that old cache stays exact, disappears, or
becomes partial before selection. Existing bytes may seed the request-local
write buffer and preserve unselected coordinates; they are a merge base, not a
dirty-work satisfaction boundary. Ordinary full HP planning may still consume
the same exact cache immediately. Dirty selection itself forms satisfaction
only from current-request external results, never from old formal cache.

RT proxy state uses signed logical HP `region_hp` but remains image-only. The
checked adapter carries one exact built-in logical ImageRect into downsample, then
subtracts the committed HP Value's data-window origin to obtain the zero-based
pixel ROI. `RealtimeProxyGraph::NodeState::region_hp` retains the logical
Region; the downscaled proxy payload and `roi_rt` remain zero-based RT storage. TensorSlice
and Whole staged validity do not create partial downsample requests. Region
values and Tensor axes are included in retained-memory accounting.

## V-13 Packed Memory Cache and Portable Disk Boundary

A formal HP `NodeOutput` may retain the complete immutable packed FP4 Value in
its existing Value authority. Ordinary cache copies preserve descriptor,
block-scale quantization, Blocked layout, exact byte envelope, allocation,
logical revision, and Ready state. `Node::hp_region` independently retains the
exact TensorSlice validity; neither fact is reconstructed from a reduced image
snapshot or inferred from storage.
This is runtime memory-cache retention, not a new persistent identity or cache
format.

The configured disk mechanism now captures every supported named Value through
the public artifact archive, but the reserved canonical `image` slot still has
to produce the configured ordinary-image projection. Before planned bytes are
admitted to `ComputeIoExecutor`, before the executor callback is created, and
before filesystem or codec work, that image must be Ready, host-readable,
image-faceted, Strided, unquantized, and compatible with the selected codec's
explicit whole-byte storage set. A packed FP4 canonical image therefore fails
with `GraphError{InvalidParameter}`. Generic and provider-defined multi-buffer
Values outside the image slot are accepted only when public artifact capture
and the active provider generation validate every descriptor, layout, binding,
and payload fact. A node with no effective nonempty image cache entry retains
its historical no-op behavior and does not enter this validation boundary.

Rejection never drops metadata, widens packed bytes, invents an image facet, or
silently skips a named Value. The current `ImageArtifactCodec` ABI remains an
auxiliary projection boundary; the public archive and versioned cache manifest
own portable replay.

## DI-1/DI-2 Observed Statistics Cache Boundary

Issue #129 defines bounded observed min/max and histogram query/result/cache-key
values. DI-2 installs `ImageStatisticsStore` as a bounded, mutex-protected
derived-result owner inside `GraphCacheService`. A complete key
contains a valid process-local `ValueRevisionId`, an optional `ContentDigest`,
the exact normalized `RegionSet`, exactly one stable `ChannelId` or
`ChannelGroupId`, algorithm, positive algorithm version, and bounded algorithm
parameters. Distinct revisions, content identities, Regions, selections,
algorithms, versions, or histogram parameters are distinct derived requests.

Each accepted task retains the exact Ready image Value, scans through
`ImageView` only, validates a result, and arbitrates cancellation against
single publication. The injected internal scheduler takes one task exactly
once and may run it inline or asynchronously; the store owns no worker or
execution policy. Exact cache hits return a ready future without scheduling.
Scan failure or cancellation publishes no entry. Deterministic oldest-entry
eviction, exact revision invalidation, and explicit clear affect derived data
only; an already accepted in-flight task may publish after clear unless its
request is explicitly cancelled.

Statistics are not fields of `Value`, `ImageFacet`, formal HP cache entries,
descriptor/content identity, disk-cache paths, or artifact manifests. Creating,
recomputing, or evicting a result cannot modify Value revision, canonical
digest, HP validity, or persisted representation. The store uses the complete
key; allocation identity, graph revision, HP/RT generation, or descriptor
digest alone is insufficient. Content digest is optional
because a valid runtime revision may be observed before content traversal is
requested.

### Current bounded mechanism and future persistence relationship

[ADR 0008](../adr/0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md)
separates future persistence into graph documents, canonical descriptor
envelopes, artifact/cache manifests plus chunks, and never-persisted runtime
state. `DescriptorDigest`, `ContentDigest`, `StorageLayoutDigest`,
`ArtifactId`, and `ValueRevisionId` answer different identity questions;
device/allocation identity, fences, leases, access plans, and residency
replicas never enter persistent logical content identity.
The current V-3 process-local `ValueRevisionId` is runtime publication identity,
not any future canonical descriptor, content, layout, or artifact digest.

DI-4 defines, and the current implementation uses, the public named-Value
archive and versioned manifest described above without changing formal cache
authority.
HP output remains the only formal reusable memory cache, RT proxy output remains
transient, and injected image/metadata codecs remain optional projection and
detached-parameter implementation boundaries. No residency replica, projection,
manifest path, or persisted runtime identity becomes a second cache authority.

V-15 does not change this cache format or authority. Its optional OpenEXR deep
adapter can read or write one provider-defined Value at a caller-selected path,
but neither operation is a graph-cache load/save, manifest/chunk transaction,
or formal HP publication. Descriptor, storage-layout, and provider-selected
ContentDigest remain generic semantic identities; the adapter does not promote
them to cache keys, paths, receipts, or durability evidence. Provider
replacement and read leases protect interpretation lifetime only. The caller,
not the provider or `ComputeIoExecutor`, owns eligibility, overwrite/commit
policy, and any later cache or output outcome.

[ADR 0009](../adr/0009-compute-io-durability-and-completion-semantics.md)
additionally separates discardable cache persistence from durable user-output
commit. Issue #88 now implements the bounded mechanism and the staged HP
cache-save vertical described above; synchronous cache administration and load
remain unchanged. The current indivisible image-codec call runs wholly on the
I/O worker. A future split codec contract must return independently admitted
CPU-heavy phases to the CPU domain. The executor never owns cache eligibility,
paths, output commit policy, Graph-document transactions, daemon state, retry,
receipts, or durability. The future post-publication cache outcome and
`OutputStore` commit authority remain separate from this executor.

## Implementation and Validation Entry Points

- `src/lib/core/image_artifact_codec.hpp`
- `src/lib/core/cache_metadata_codec.hpp`
- `src/lib/adapters/opencv/image_artifact_codec_opencv.*`
- `src/lib/adapters/yaml/yaml_cache_metadata_codec.*`
- `src/lib/adapters/yaml/parameter_value_yaml.*`
- `src/lib/providers/configured_image_artifact_codec.*`
- `src/lib/providers/configured_persistence_adapters.*`
- `src/lib/core/{sample_conversion,value_artifact}.*`
- `src/lib/adapters/{opencv,openexr}/`
- `include/photospider/data/packed_dense_tensor_view.hpp`
- `include/photospider/memory/blocked_layout.hpp`
- `include/photospider/data/region.hpp`
- `src/lib/core/region.*`
- `src/lib/core/region_image_adapter.*`
- `src/lib/graph/graph_cache_service.*`
- `src/lib/execution/device/compute_io_executor.*`
- `src/lib/adapters/openexr/openexr_deep_scanline_adapter.*`
- `tests/integration/test_openexr_deep_scanline_provider.cpp`
- `src/lib/graph/graph_model.*`
- `src/lib/runtime/kernel_compute.cpp`
- `src/lib/ipc/output_store.*`
- `src/lib/compute/request/compute_cache_policy.*`
- `src/lib/compute/dispatch/compute_node_task_runner.*`
- `src/lib/compute/dispatch/compute_task_dispatcher.*`
- `src/lib/compute/dirty/realtime_proxy_graph.*`
- `src/lib/compute/dirty/dirty_write_buffers.*`
- `tests/integration/test_cpu_dense_tensor_image_operation.cpp`
- `tests/integration/test_packed_fp4_dense_tensor.cpp`
- `tests/unit/test_region_contracts.cpp`
- `tests/integration/test_disk_cache_diagnostic_concurrency.cpp`
- `tests/unit/test_compute_io_executor.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_host_adapter.cpp`
- `tests/integration/dependency_disabled_install_smoke.py`
