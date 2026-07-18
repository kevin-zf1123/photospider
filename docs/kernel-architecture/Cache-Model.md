# Kernel Cache Model

The kernel has formal HP memory cache on each `Node`, transient RT proxy state
in `RealtimeProxyGraph`, and disk cache files under the graph cache root. This
document defines the current cache semantics.

## Formal Cache and Transient State

| Location | Status | Meaning |
| --- | --- | --- |
| `Node::cached_output_high_precision` | Formal cache | Full-quality reusable HP output. |
| `RealtimeProxyGraph` node state | Transient RT proxy | Low-resolution interactive preview/update output. |

Only high-precision output is formal reusable cache. That means only HP output
may be used as the authoritative source for subsequent HP compute, disk cache,
long-term storage, and other reusable cache behavior. RT proxy output is
transient interactive state and must not be treated as authoritative cache, as a
disk-cache synchronization source, or as long-term storage input.

## HP Cache

HP compute writes `cached_output_high_precision`. HP cache is the authoritative
full-quality result for a node.

Associated fields:

| Field | Meaning |
| --- | --- |
| `hp_version` | Version counter for HP output changes. |
| `hp_roi` | Most recent or merged HP region updated. |

## RT State

RT compute writes `RealtimeProxyGraph`. Each proxy node is keyed by the original
graph node id and stores only low-resolution output, HP-space ROI metadata,
version, and RT dirty-source generation. It does not copy Node parameters,
inputs, topology, caches, or formal HP state. When the observed graph topology
generation changes, synchronization resets live proxy entries rather than
preserving state by reused node id, so reload/edit workflows cannot expose stale
low-resolution output from an earlier graph.

Dirty RT execution does not write graph-owned RT fields. Worker tasks stage
proxy output, ROI metadata, version counters, and dirty-source commit
generation in `RealtimeProxyWriteBuffer`, then commit that staged state to
`RealtimeProxyGraph` after the RT dirty work set drains. Dirty HP execution
similarly stages HP output in `HighPrecisionDirtyWriteBuffer` before committing
to `GraphModel`, so HP/RT siblings can compute concurrently while preserving
RT-first commit ordering.

Associated fields:

| Proxy field | Meaning |
| --- | --- |
| `version` | Version counter for RT proxy output changes. |
| `roi_hp` | Most recent or merged HP-space region represented by RT update. |
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

Image bytes cross the private, dependency-neutral `ImageArtifactCodec` contract.
`Kernel` obtains one configured shared codec from the product composition root
and injects it into `GraphCacheService`; Graph/cache code supplies only paths,
`ImageBuffer`, and normalized integer precision. The current production adapter
uses OpenCV imgcodecs and translates provider failures to `GraphErrc::Io`, while
OpenCV `StsNoMem` remains `std::bad_alloc`. Tests inject a deterministic fake to
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
recording the latest diagnostic through GraphModel's dedicated disk-cache
diagnostic mutex. Callers inspect that state through a snapshot API instead of
reading mutable optional storage directly. The diagnostic result distinguishes
skipped attempts, true misses, hits, and read/parse errors. Bad image files,
invalid YAML metadata, and filesystem failures are recorded as errors with an
error code and message instead of being indistinguishable from a normal cache
miss.

## Cache Commands

| Operation | Effect |
| --- | --- |
| Clear drive cache | Remove disk cache directory contents and recreate root. |
| Clear memory cache | Clear in-memory HP cache tracked by `GraphModel`. |
| Clear cache | Clear both disk and memory cache. |
| Cache all nodes | Save nodes with HP output to disk when configured. |
| Free transient memory | Clear non-ending node memory cache state. |
| Synchronize disk cache | Save HP output and remove stale disk files for nodes without HP output. |

Disk cache save, load, and synchronization use `cached_output_high_precision`
only. RT proxy output does not protect stale disk files and is not promoted to
disk cache state.

## Boundaries and Rationale

- HP paths write `cached_output_high_precision`.
- RT paths write `RealtimeProxyGraph` as transient interactive state, using
  `RealtimeProxyWriteBuffer` for dirty worker writes before proxy commit.
- Formal cache save/load/sync behavior, subsequent HP compute, and long-term
  storage must use HP output and must not promote RT output to authoritative
  cache.
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
Issue #62 completes this runtime/cache value boundary. The normal configured
product still discovers and links yaml-cpp for its concrete adapters; the
dependency-disabled product/build evidence remains Issue #63.
[ADR 0002](../adr/0002-external-libraries-are-kernel-adapters.md) and the exact
[dependency-neutral kernel target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel)
describe the final adapter and document boundary.

## Implementation and Validation Entry Points

- `src/lib/core/image_artifact_codec.hpp`
- `src/lib/core/cache_metadata_codec.hpp`
- `src/lib/adapters/opencv/image_artifact_codec_opencv.*`
- `src/lib/adapters/yaml/yaml_cache_metadata_codec.*`
- `src/lib/adapters/yaml/parameter_value_yaml.*`
- `src/lib/providers/configured_image_artifact_codec.*`
- `src/lib/graph/graph_cache_service.*`
- `src/lib/graph/graph_model.*`
- `src/lib/compute/realtime_proxy_graph.*`
- `src/lib/compute/dirty_write_buffers.*`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_host_adapter.cpp`
