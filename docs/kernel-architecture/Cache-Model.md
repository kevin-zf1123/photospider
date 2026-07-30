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
| `region_hp` | Normalized HP-space ImageRect Region represented by RT update. |
| `dirty_source_generation` | RT dirty source generation committed for stale source checks. |

## Disk Cache

`GraphCacheService` handles disk cache files under `GraphModel::cache_root`.
Node cache entries describe cache type and location. Image cache files are saved
as image files, and named `NodeOutput::data` entries are saved as YAML metadata
next to the image file. In-memory named data remains a detached
`plugin::ParameterMap`; `GraphCacheService` never constructs YAML values.

For CLI-loaded graphs, `GraphModel::cache_root` is configured from
`cache_root_dir` before graph load and resolves to
`<cache_root_dir>/<graph_name>`. Relative `cache_root_dir` values are relative
to the process working directory. Direct `Kernel::load_graph` callers that do
not provide a cache root continue to use `<root_dir>/<graph_name>/cache`.

Disk cache precision currently supports `int8` and `int16` save paths. Loaded
image cache data is converted into float image buffers.

The current disk format does not persist Region metadata, so only a complete HP
output may be saved or protect configured disk artifacts during
synchronization. A partial formal output is not loaded over and is never
relabelled as complete. Saving or synchronizing a partial node removes older
configured image/YAML artifacts instead of encoding partial bytes. A successful
disk load derives complete validity for the freshly decoded output.

Image bytes cross the private, dependency-neutral `ImageArtifactCodec` contract.
`Kernel` obtains one configured shared codec from the product composition root
and injects it into `GraphCacheService`; Graph/cache code supplies only paths,
`ImageBuffer`, and normalized integer precision. With OpenCV enabled, the
configured adapter uses OpenCV imgcodecs and translates provider failures to
`GraphErrc::Io`, while OpenCV `StsNoMem` remains `std::bad_alloc`. With OpenCV
disabled, the configured unavailable codec returns `GraphErrc::Io` without
discovering or exporting OpenCV. Tests inject a deterministic fake to
verify call order, lifetime retention, precision selection, recoverable errors,
and resource exhaustion without reading or writing a real image format.

Named values independently cross the private, dependency-neutral
`CacheMetadataCodec` contract as paths and detached `ParameterMap` values.
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
read/parse errors. Bad image files, invalid YAML metadata, and filesystem
failures are recorded as errors with an error code and message instead of being
indistinguishable from a normal cache miss.

## Current Durability and Failure Boundary

Current cache save is not an atomic cache-entry transaction.
`GraphCacheService` creates directories and invokes the configured image and
metadata codecs against their final sibling paths. The image payload and YAML
metadata can therefore succeed or fail independently. The service provides no
entry-level staging rename, rollback, manifest-last publication, file or
directory synchronization receipt, retry protocol, or crash recovery.

`cache_all_nodes` counts nodes with present HP output for which the save path
was attempted; the count is not proof that each node had a configured artifact
or that a durable cache entry exists. Cache load diagnostics are process-memory
observations of the latest attempt, not durable audit records.

The current product compute commit policy performs eligible changed-HP cache
writes after exact revision/generation validation but before the no-throw live
Graph swap. A cache codec, filesystem, or allocation failure can therefore
fail that `ComputeRun` and leave live Graph/RT state unchanged. This ordering
is current behavior, not a statement that cache is part of user-output commit.
[ADR 0009](../adr/0009-compute-io-durability-and-completion-semantics.md)
accepts a different target ordering in which cache persistence has an
independent typed outcome after Run publication.

## Cache Commands

| Operation | Effect |
| --- | --- |
| Clear drive cache | Remove disk cache directory contents and recreate root. |
| Clear memory cache | Clear in-memory HP cache tracked by `GraphModel`. |
| Clear cache | Clear both disk and memory cache. |
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

A formal HP `NodeOutput` may carry both `image_value` and `image_buffer`.
For a nonempty CPU image, a valid sealed Value is the allocation/revision
identity authority; the ImageBuffer is an independently owned compatibility
snapshot. Ordinary HP publication and cache-load boundaries normalize a
missing Value from the current CPU ImageBuffer before the output becomes
formal cache. Copying a formal cache entry preserves its
`AllocationIdentity` and `ValueRevisionId`.

Mutable dirty work cannot retain the old authority. Its clone clears
`image_value` before any ImageBuffer write and seals the settled bytes into a
fresh allocation and revision before HP commit. Replacement output and disk
decode likewise create fresh identities. RT proxy output remains transient and
does not become a formal cache identity source.

Disk save prefers the sealed Value when present and derives a temporary
ImageBuffer snapshot from its checked image view, so later mutation of the
compatibility snapshot cannot change the persisted bytes. The existing image
and YAML formats still persist only representation bytes and named metadata:
neither `AllocationIdentity` nor `ValueRevisionId` is serialized, reconstructed
from a path, or used as a persistent cache/task key. Both tokens are opaque,
process-local runtime identities; a disk reload necessarily mints new ones.

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
replaces it and derives complete validity. Generic ABI v2 monolithic callbacks
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

RT proxy state uses HP-space `region_hp` but remains image-only. The checked
adapter derives current rectangular downsample/inspection metadata only from
one exact built-in ImageRect. TensorSlice and Whole do not enter the RT or
downsample rectangle boundary. Region values and Tensor axes are included in
retained-memory accounting.

### Accepted future persistence relationship (not current behavior)

[ADR 0008](../adr/0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md)
separates future persistence into graph documents, canonical descriptor
envelopes, artifact/cache manifests plus chunks, and never-persisted runtime
state. `DescriptorDigest`, `ContentDigest`, `StorageLayoutDigest`,
`ArtifactId`, and `ValueRevisionId` answer different identity questions;
device/allocation identity, fences, leases, access plans, and residency
replicas never enter persistent logical content identity.
The current V-3 process-local `ValueRevisionId` is runtime publication identity,
not any future canonical descriptor, content, layout, or artifact digest.

This target does not change the current cache format or authority described
above. HP output remains the only formal reusable cache, RT proxy output
remains transient, and the current injected artifact/metadata codecs remain
the implementation boundary until later slices migrate cache manifests and
payloads. No future residency replica becomes a second cache authority.

[ADR 0009](../adr/0009-compute-io-durability-and-completion-semantics.md)
additionally separates discardable cache persistence from durable user-output
commit. Its accepted Issue #88 `ComputeIoExecutor` may own bounded cache,
asset, and codec I/O mechanism work, with both operation-count and
retained-byte admission; CPU-heavy codec phases return to the existing CPU
domain. It never owns output commit policy, Graph-document transactions,
daemon state, paths, retry, or durability. The future `OutputStore` commit
authority and its typed receipt remain separate from cache and from this
executor.

## Implementation and Validation Entry Points

- `src/lib/core/image_artifact_codec.hpp`
- `src/lib/core/cache_metadata_codec.hpp`
- `src/lib/adapters/opencv/image_artifact_codec_opencv.*`
- `src/lib/adapters/yaml/yaml_cache_metadata_codec.*`
- `src/lib/adapters/yaml/parameter_value_yaml.*`
- `src/lib/providers/configured_image_artifact_codec.*`
- `src/lib/providers/configured_persistence_adapters.*`
- `src/lib/core/value_image_adapter.*`
- `include/photospider/data/region.hpp`
- `src/lib/core/region.*`
- `src/lib/core/region_image_adapter.*`
- `src/lib/graph/graph_cache_service.*`
- `src/lib/graph/graph_model.*`
- `src/lib/runtime/kernel_compute.cpp`
- `src/lib/ipc/output_store.*`
- `src/lib/compute/compute_cache_policy.*`
- `src/lib/compute/compute_node_task_runner.*`
- `src/lib/compute/compute_task_dispatcher.*`
- `src/lib/compute/realtime_proxy_graph.*`
- `src/lib/compute/dirty_write_buffers.*`
- `tests/integration/test_cpu_dense_tensor_image_operation.cpp`
- `tests/unit/test_region_contracts.cpp`
- `tests/integration/test_disk_cache_diagnostic_concurrency.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_host_adapter.cpp`
- `tests/integration/dependency_disabled_install_smoke.py`
