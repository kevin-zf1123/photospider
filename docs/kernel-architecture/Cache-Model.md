# Kernel Cache Model

The kernel has memory cache fields on each `Node` and disk cache files under
the graph cache root. This document defines the intended cache semantics.

## Formal Cache and Transient State

| Field | Status | Meaning |
| --- | --- | --- |
| `cached_output_high_precision` | Formal cache | Full-quality reusable HP output. |
| `cached_output_real_time` | Transient RT state | Interactive preview/update output. |

Only high-precision output is formal reusable cache. That means only HP output
may be used as the authoritative source for subsequent HP compute, disk cache,
long-term storage, and other reusable cache behavior. `cached_output_real_time`
is transient interactive state and must not be treated as authoritative cache,
as a disk-cache synchronization source, or as long-term storage input.

## HP Cache

HP compute writes `cached_output_high_precision`. HP cache is the authoritative
full-quality result for a node.

Associated fields:

| Field | Meaning |
| --- | --- |
| `hp_version` | Version counter for HP output changes. |
| `hp_roi` | Most recent or merged HP region updated. |

## RT State

RT compute writes `cached_output_real_time`. RT state is the interactive preview
or proxy result. It may be lower resolution than HP output and is not formal
cache authority.

Dirty RT execution does not write graph-owned RT fields directly from worker
tasks. It stages proxy output, ROI metadata, version counters, and dirty-source
commit generation in `RealtimeDirtyWriteBuffer`, then commits that staged state
to `GraphModel` after the RT dirty work set drains. This keeps the current
`DirectGraphCommit` path deterministic while leaving room for a later buffered
commit policy to restore cross-intent HP/RT sibling concurrency.

Associated fields:

| Field | Meaning |
| --- | --- |
| `rt_version` | Version counter for RT output changes. |
| `rt_roi` | Most recent or merged HP-space region represented by RT update. |

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
| Clear memory cache | Clear in-memory HP cache and RT transient state currently tracked by the service. |
| Clear cache | Clear both disk and memory cache. |
| Cache all nodes | Save nodes with HP output to disk when configured. |
| Free transient memory | Clear non-ending node memory cache state. |
| Synchronize disk cache | Save HP output and remove stale disk files for nodes without HP output. |

Disk cache save, load, and synchronization use `cached_output_high_precision`
only. RT output does not protect stale disk files and is not promoted to disk
cache state.

## Cache Rules

- New HP code writes `cached_output_high_precision`.
- New RT code writes `cached_output_real_time` as transient interactive state,
  using `RealtimeDirtyWriteBuffer` for dirty worker writes before graph commit.
- Formal cache save/load/sync behavior, subsequent HP compute, and long-term
  storage must use HP output and must not promote RT output to authoritative
  cache.
- Tests should verify HP and RT fields independently.

`GraphInspectService` selects a display source in HP, then RT order and labels
the selected source explicitly. RT metadata may be shown for inspection, but it
is labeled as transient state rather than formal cache authority.
