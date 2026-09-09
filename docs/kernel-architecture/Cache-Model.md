# Cache Model

`PlanCacheKey` remains a non-security physical-plan identity and excludes input
payload. It does not validate stale plans or identify execution results.

Package 0.6 provides opt-in ExecutionContext result retention with
`result_cache_bytes`, a sublimit of `maximum_live_bytes`. Copies share immutable
allocation leases, eviction releases only cache references, and strict working
admission reclaims optional entries first. `clear_result_cache()` invalidates
retention epochs; active readers remain valid and old producers cannot refill
a cleared epoch. Cache statistics expose hits, misses, evictions, sharing,
entries and retained capacity. Zero cache bytes preserves uncached execution.

InputSnapshotStore owns independently bounded immutable image/mask blocks.
Import validates the profile; patch copies intersecting blocks and preserves
old snapshots. `content_identity(region)` is canonical SHA-256 over metadata
and demanded sample bits, independent of block geometry and allocation. Exact
signed-zero bits remain distinct. Snapshot bindings provide regional reads.

Result keys recursively cover each consumer's demanded producer regions,
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
Its lifetime is independent of graph replacement/destruction. Ordinary plan
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

The finite format stores Float32 HW masks or profiled HWC RGBA regions, a
version, exact expected metadata and key, and SHA-256 over metadata plus packed
little-endian sample bits. Allocation size comes from the validated plan, never
file-supplied lengths. Header/size/hash/numeric mismatch is a disposable miss.
Writes complete in a temporary file before rename; no durable commit/recovery
claim is made. Write failure/queue pressure skips retention without failing the
computed result. `flush_disk_cache()` is an explicit caller operation outside
publication; destruction also joins the writer. `clear_disk_cache()` removes
entries and invalidates pending write epochs.

`test_disk_cache` runs separate processes for initial write, reuse, header,
length, checksum and version corruption, deletion/rebuild, failed writes and
strict quota/queue-pressure cases. See [ADR 0018](../adr/0018-local-result-caches-and-frozen-execution.md).

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
