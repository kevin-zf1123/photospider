# Cache Model

`PlanCacheKey` remains a non-security physical-plan identity and excludes input
payload. It does not validate stale plans or identify execution results.

Package 0.5 additionally provides opt-in ExecutionContext result retention with
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
descendants. Only deterministic, side-effect-free, cacheable CPU work qualifies.

Bounded shared coordinators merge identical in-flight regional computations;
CPU work stays in the existing callback pool. Each waiting caller independently
observes its own cancellation/currentness. Last-subscriber cancellation drains
the producer before returning. Explicit producer snapshots own their inputs
and registry independently of caller stack and editable graph state.

FrozenExecution captures a current plan and immutable Value/snapshot bindings.
Its lifetime is independent of graph replacement/destruction. Ordinary plan
execution retains stale checks. `for_region` derives a pinned output tile.
Custom RegionalSource callbacks must be imported before freeze.

The S3 disk contract is accepted in [ADR 0018](../adr/0018-local-result-caches-and-frozen-execution.md);
its implementation and restart acceptance remain tracked by #276.
