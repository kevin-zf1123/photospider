# S3: Editable Cached Image Workflows

The standalone [example](../../examples/s3_image_workflow) uses only installed
public APIs. It combines immutable input snapshots, Gaussian blur, runtime
exposure, an independent mask and source-over. It also supplies hard circle
stamps and box-scaled previews. The default fixture is 17x13 linear-sRGB
premultiplied Float32 RGBA, with an HW Float32 mask.

## Build and run

From the repository root, install the built kernel and configure the independent
example. These commands do not need a daemon:

```sh
cmake --build build/issue257-static --target photospider -j 8
cmake --install build/issue257-static --prefix build/s3-prefix
cmake -S examples/s3_image_workflow -B build/s3-example-installed \
  -DCMAKE_PREFIX_PATH="$PWD/build/s3-prefix"
cmake --build build/s3-example-installed -j 8
build/s3-example-installed/photospider_s3_image_workflow --scenario cache
build/s3-example-installed/photospider_s3_image_workflow --scenario preview
```

The independently buildable `plugins/ops/rgba32f` package consumes the same
prefix. Pass its trusted module path with `--module PATH` to run cache and
preview scenarios through the C operation ABI. Disk persistence currently
requires the verifiable built-in registry; custom/C-module registries retain
process-local result caching.

For disk acceptance, use a dedicated cache directory. Each command starts a
new process; corrupted or cleared entries are rebuilt from the same source:

```sh
build/s3-example-installed/photospider_s3_image_workflow \
  --scenario disk-write --cache-dir "$PWD/build/s3-derived-cache"
build/s3-example-installed/photospider_s3_image_workflow \
  --scenario disk-read --cache-dir "$PWD/build/s3-derived-cache"
build/s3-example-installed/photospider_s3_image_workflow \
  --scenario disk-corrupt --cache-dir "$PWD/build/s3-derived-cache"
build/s3-example-installed/photospider_s3_image_workflow \
  --scenario disk-clear --cache-dir "$PWD/build/s3-derived-cache"
```

## Checkable results

| Scenario | Required observation |
| --- | --- |
| `cache` / S3Cache.LocalInvalidation | `blur_after_gain=0 patch_blur_tiles=6 unrelated_callbacks=0 oracle=passed` |
| `preview` / S3Preview.LatestAndExport | `stamps=9 export_tiles=20 preview_tiles=27 rejected=3 quality=full oracle=passed` |
| `disk-write` | `oracle=passed`; valid entries remain after exit |
| `disk-read` | Positive disk hits and `oracle=passed` |
| `disk-corrupt` | Positive invalid-entry count, successful recomputation and `oracle=passed` |
| `disk-clear` | Recomputed result still passes the independent oracle |

Input stamps are checked against a separate ordered-circle formula. Formal
images are checked against a full-image two-dimensional Gaussian/composition
oracle with `atol=1e-6, rtol=1e-5`. Preview is deliberately approximate; full
quality and exports retain original blur parameters. Counters are deterministic
work observations, not timing promises or request-to-display measurements.

## Modify and compose

`scene.hpp` defines the workflow and inputs. Change the fixture, operation
connections, static radius/sigma or preview factor there. The factor-four proxy
uses image/mask downsampling before blur/composition; blur radius is ceil-scaled
with minimum one, and sigma is scaled with minimum 0.1. Numeric gain, brush
center/radius/color/alpha and new pixel snapshots are runtime bindings. They
reuse their compiled plan; a graph edit recompiles its changed document.
`FrozenExecution::for_region` derives output work without reanalysis.

`InputSnapshotStore::import_value` establishes immutable blocks, and `patch`
returns a new version while sharing unchanged blocks. ExecutionBinding selects
exactly one ordinary Value, RegionalSource or InputSnapshot. Frozen capture
accepts Values and kernel snapshots; custom RegionalSource data must first be
imported. Snapshot capacity has its own explicit budget, including old versions.

`coordinator.hpp` is application policy, not an installed request/Job API. Its
single-threaded event loop has an eight-stamp FIFO and a coalesced pending gain.
A rejected full-queue stamp can be retried after progress. Each tick applies at
most one edit and executes one preview or export tile, alternating active
consumers. All admitted stamps remain ordered. Preview completion checks target,
content version and quality; the export retains its initially frozen inputs.
The application owns the assembled frame buffers outside kernel byte accounting.

Set `ExecutionContextConfig::result_cache_bytes` to enable memory reuse; zero
is the default. Cache retained allocations remain within maximum_live_bytes.
The example uses a 1 MiB controlled execution budget and 256 KiB cache sublimit.
DiskCacheConfig explicitly selects an exclusive directory and finite limits.
Writes are optional and asynchronous. `flush_disk_cache` is used only for the
explicit disk handoff, never by preview publication. Clearing either cache is
always compatible with correct recomputation.

## Validation boundaries

The test suite also covers odd sizes, image/mask scaling, parameter errors,
partial patches, retained snapshots, ordinary stale rejection, frozen export,
independent and last-subscriber cancellation, cache-clear races and strict
storage limits. Separate disk processes exercise header/length/hash/version
corruption, write failures, eviction and queue pressure. Static/shared installed
consumers run the example with both C++ and C operations. Native GPU, GUI,
pressure dynamics, document saving, durable jobs and daemon wire expansion are
outside this S3 delivery.
