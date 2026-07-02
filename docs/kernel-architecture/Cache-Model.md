# Kernel Cache Model

The kernel has formal HP memory cache on each `Node`, transient RT proxy state
in `RealtimeProxyGraph`, and disk cache files under the graph cache root. This
document defines the intended cache semantics.

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
next to the image file.

For CLI-loaded graphs, `GraphModel::cache_root` is configured from
`cache_root_dir` before graph load and resolves to
`<cache_root_dir>/<graph_name>`. Relative `cache_root_dir` values are relative
to the process working directory. Direct `Kernel::load_graph` callers that do
not provide a cache root continue to use `<root_dir>/<graph_name>/cache`.

Disk cache precision currently supports `int8` and `int16` save paths. Loaded
image cache data is converted into float image buffers.

Disk cache load attempts preserve the legacy try-load bool contract while also
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

## Cache Rules

- New HP code writes `cached_output_high_precision`.
- New RT code writes `RealtimeProxyGraph` as transient interactive state, using
  `RealtimeProxyWriteBuffer` for dirty worker writes before proxy commit.
- Formal cache save/load/sync behavior, subsequent HP compute, and long-term
  storage must use HP output and must not promote RT output to authoritative
  cache.
- Tests should verify HP graph cache and RT proxy graph state independently.

`GraphInspectService` selects node-local display metadata from HP cache only.
RT proxy inspection can be added as a separate frontend-facing view without
embedding RT state back into `GraphModel`.
