# Cache Model

`PlanCacheKey` remains a non-security physical-plan identity and excludes input
payload. It does not validate stale plans or identify execution results.

Package 0.7 provides opt-in ExecutionContext result retention with
`result_cache_bytes`, a sublimit of `maximum_live_bytes`. Copies share immutable
allocation leases, eviction releases only cache references, and strict working
admission reclaims optional entries first. `clear_result_cache()` invalidates
retention epochs; active readers remain valid and old producers cannot refill
a cleared epoch. Cache statistics expose hits, misses, evictions, sharing,
entries and retained capacity. Zero cache bytes preserves uncached execution.

InputSnapshotStore owns independently bounded immutable Float32 image-v2 and
canonical coverage-mask blocks. RGB/RGBA/XYZ/Lab images retain their ordered
channel roles, reference white, units and alpha association; HWC C comes from
the descriptor. Imports and patches validate complete image channels and finite
samples, including signed/HDR and straight hidden colors. Patches require exact
descriptor/facets, copy intersecting blocks and preserve old snapshots. `content_identity(region)` is canonical SHA-256 over metadata
and demanded sample bits, independent of block geometry and allocation. Exact
signed-zero bits remain distinct. Snapshot bindings provide regional reads.

Memory and native completed Values retain their actual facets. Result hits
revalidate resolved descriptor, demanded coverage, output semantic rules and
typed sample constraints before reuse, including the flight-completion race
lookup. Numeric validation establishes nearest/gradual-underflow arithmetic and
restores the caller's floating environment. Generic Drop outputs may publish
opaque facets; PreserveInput chains originating at that dynamic boundary retain
this capability while still rejecting unproven typed facets. Known declaration
and explicit output semantics require exact facet equality. Invalid computed typed values remain OperationFailed. Input-copy keys
already include complete metadata and preserve it on native publication.

Result-region keys v3 recursively cover each consumer's demanded producer regions,
operation semantics/parameters and stable input content. Unrelated graph edits
and node identifiers do not invalidate unchanged content. Whole dependencies
remain conservative; scalar changes invalidate dependent output. Generic
unproven input sources remain executable but disable cross-Run reuse for their
descendants. Only deterministic, side-effect-free, cacheable work with a proven implementation qualifies.

Bounded shared coordinators merge identical in-flight regional computations;
CPU work stays in the existing callback pool. Each waiting caller independently
observes its own cancellation/currentness. Last-subscriber cancellation drains
the producer before returning. Explicit producer snapshots own their inputs
and registry independently of caller stack and editable graph state.

FrozenExecution captures a current plan and immutable Value/snapshot bindings.
It is an in-memory owned object, with no serialized-plan reader. Its lifetime
is independent of graph replacement/destruction; capture copies snapshot handles
so later replacement of a caller-owned handle cannot alter frozen inputs. Ordinary plan
execution retains stale checks. `for_region` derives a pinned output tile.
Custom RegionalSource callbacks must be imported before freeze.

## Disposable disk regions

An explicit `ExecutionContextConfig::disk_cache` requires positive result cache
capacity. DiskCacheConfig sets the directory, total byte/entry limits and a
bounded write queue. One context exclusively locks the directory. The cache
contains only `.pscache` files named by canonical SHA-256 keys and disposable
`.tmp` writes; unrelated names are ignored. Bytes include an active write
reservation. Pending writes retain the original accounted immutable buffers
and can be dropped under computation pressure.

Persistent eligibility is restricted to `make_default_operation_registry()`.
Its implementation fingerprint covers maintained source/headers, compiler,
platform and build options. Custom and C-module registries retain process-local
cache support but publish no persistent implementation identity. This is a
correctness identity, not native-code trust or a security signature.

Disk format 2 (`PSCACHE2`, disk-result key domain v2) stores Float32 HW coverage
masks or supported HWC image-v2 regions. The header encodes dtype, shape,
Region, exact canonical facet keys/versions/payloads, byte count and key. SHA-256
covers this header and packed little-endian Float32 sample bits. Old format 1
is a miss. Reads compare the complete expected header, publish the stored
validated facets and revalidate typed samples; no default RGBA facet is rebuilt. Allocation size comes from the validated plan, never
file-supplied lengths. Header/size/hash/numeric mismatch is a disposable miss.
Writes complete in a temporary file before rename; no durable commit/recovery
claim is made. Write failure/queue pressure skips retention without failing the
computed result. `flush_disk_cache()` is an explicit caller operation outside
publication; destruction also joins the writer. `clear_disk_cache()` removes
entries and invalidates pending write epochs.

`test_disk_cache` runs separate processes for initial write, reuse, header,
length, checksum and version corruption, deletion/rebuild, failed writes and
strict quota/queue-pressure cases. `test_input_snapshot` and the typed disk
regressions exercise RGB/BGR, straight/premultiplied RGBA, XYZ/XYZA and Lab/LabA,
including D65/D50 metadata separation, three-channel patches, retained frozen
inputs, cold/warm reuse and restart. `tests/support/typed_images.hpp` provides
the public WorkflowDocument/compile/execute identity example and checkable
signed/HDR/negative-zero sample payloads. See [ADR 0018](../adr/0018-local-result-caches-and-frozen-execution.md).

## S4 native retention

MetalFp32 result keys additionally separate the numeric mode, planned backend,
compiled implementation fingerprint and context device generation. A fallback
result and its descendants never populate the expected native result keys.
The current frozen registry fixes C-module implementation ownership for the
context. CPU exact results cannot be replaced by approximately computed native
results. Loss of the device disables native keys and clears retained entries.

Completed native input copies share the same bounded LRU. Their keys hash the
actual demanded logical sample bytes, descriptor, facets, Region and device;
they do not rely on mutable addresses or caller revision claims. This can avoid
another native upload even for immutable ordinary Value bindings. It does not
make an arbitrary RegionalSource eligible for computed-result caching; such a
source is read again before its actual bytes authorize input-copy reuse.

`native_retained_bytes` counts unique native owners within `retained_bytes`.
`native_upload_hits` reports avoided uploads. Copies, result retention and
active readers all retain their original controlled allocation leases. Clear
and eviction retire eligibility without invalidating borrowed active data.
Native shared computations keep the independent/last-subscriber cancellation
rules. Shared followers report shared work without counting the producer's
native dispatches and transfers a second time.

Disk read and write are restricted to CpuExact execution in this implementation,
including when an explicitly Metal plan falls back to CPU. CPU disk behavior
otherwise remains unchanged. Native implementations and generated shader inputs
participate in maintained build identity; generated build headers are not source
assets. `test_native_cache` and its C-module variant cover reuse, edits, numeric
mode/fallback isolation, cancellation, clear races and bounded retained capacity.

Focused validation using the existing build directory:

```sh
cmake --build build/issue257-static --target test_input_snapshot test_disk_cache test_result_cache test_frozen_execution test_native_cache -j 8
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --test-dir build/issue257-static -R '^test_(input_snapshot|disk_cache|result_cache|frozen_execution|native_cache(_plugin)?)$' --output-on-failure
```

Typed native residency uses a public pure byte-copy operation: each of the nine
image descriptors must preserve exact bytes/facets, dispatch once cold and zero
times on a cache hit. This validates storage and reuse without introducing a
color conversion operation. Existing native cancellation and budget cases remain.
