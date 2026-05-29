# Kernel Cache Model

The kernel has memory cache fields on each `Node` and disk cache files under
the graph cache root. This document defines the intended cache semantics.

## Formal Memory Caches

| Field | Status | Meaning |
| --- | --- | --- |
| `cached_output_high_precision` | Formal | Full-quality HP output. |
| `cached_output_real_time` | Formal | Interactive RT output. |
| `cached_output` | Migration residue | Old mistaken name for HP output. |

`cached_output` is not a separate long-term cache kind. It is the old name for
the HP cache and should be migrated to `cached_output_high_precision`.

## HP Cache

HP compute writes `cached_output_high_precision`. HP cache is the authoritative
full-quality result for a node.

Associated fields:

| Field | Meaning |
| --- | --- |
| `hp_version` | Version counter for HP output changes. |
| `hp_roi` | Most recent or merged HP region updated. |

## RT Cache

RT compute writes `cached_output_real_time`. RT cache is the interactive preview
or proxy result. It may be lower resolution than HP output.

Associated fields:

| Field | Meaning |
| --- | --- |
| `rt_version` | Version counter for RT output changes. |
| `rt_roi` | Most recent or merged HP-space region represented by RT update. |

## Legacy HP Name

`cached_output` exists for legacy compute paths and current migration support.
It should be treated as a mistaken HP cache name. Migration should move HP reads
and writes to `cached_output_high_precision`, then remove mirroring once callers
are updated.

## Disk Cache

`GraphCacheService` handles disk cache files under `GraphModel::cache_root`.
Node cache entries describe cache type and location. Image cache files are saved
as image files, and named `NodeOutput::data` entries are saved as YAML metadata
next to the image file.

Disk cache precision currently supports `int8` and `int16` save paths. Loaded
image cache data is converted into float image buffers.

## Cache Commands

| Operation | Effect |
| --- | --- |
| Clear drive cache | Remove disk cache directory contents and recreate root. |
| Clear memory cache | Clear in-memory cache state currently tracked by the service. |
| Clear cache | Clear both disk and memory cache. |
| Cache all nodes | Save cached node outputs to disk when configured. |
| Free transient memory | Clear non-ending node memory cache state. |
| Synchronize disk cache | Save memory cache and remove stale disk files. |

Some cache commands still operate on the old `cached_output` path. This is a
known migration area.

## Migration Rules

- New HP code writes `cached_output_high_precision`.
- New RT code writes `cached_output_real_time`.
- New code should not require `cached_output`.
- Compatibility fallbacks may read `cached_output` only while HP call sites
  migrate to `cached_output_high_precision`.
- Tests should verify HP and RT fields independently.

`GraphInspectService` currently picks cached output in legacy order
(`cached_output`, then HP, then RT) and formats debug/spatial metadata. After
the legacy HP name is removed, inspect behavior should be redefined to show HP,
RT, or both explicitly.
