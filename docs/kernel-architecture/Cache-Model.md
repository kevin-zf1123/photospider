# Cache model

## Scope and ownership

The kernel keeps separate identities for compiled plans, immutable input snapshots, completed structured Result reuse, and native backend input-copy caches. Cache membership grants reuse eligibility; active Results, snapshots, typed backing Values, and read windows retain their own owners and resource leases after an entry is cleared or evicted.


Each structured Run owns a compiled `StructuredResultCache` component for completed-result keys, manifests, source proofs, replay, snapshot restoration, and its optional cache-work quota. The lookup takes a strong manifest candidate while it is resident. That candidate remains owned through proof, replay, and adoption even if the LRU evicts it meanwhile. Adoption does not require another LRU-residency proof; an optional lookup may promote the entry only when the same manifest is still resident. Cache closing or an epoch change can still reject adoption, and the independently held candidate keeps its Result backing alive until that attempt releases it.

## Immutable Value snapshots

```cpp
#include "photospider/data/input_snapshot.hpp"

namespace ps {
Result<InputSnapshot> import_snapshot(const InputSnapshotStore& store,
                                      const Value& value) {
  return store.import_value(value);
}
}
```

`InputSnapshotStore` retains immutable rank-1 through rank-8 blocks for supported built-in numeric Value types. Import preserves valid generic bit patterns and validates supported typed scalar/tensor samples. The store bounds block count, actual payload bytes across versions, and directory metadata separately. Caller-owned Values and snapshot metadata are outside the retained-payload byte limit.

Patches require matching type, shape, and facets and replace one exact nonempty Region. The store copies intersecting blocks and preserves earlier versions. Reads and patches accept cancellation and sample bounds. A cancelled read may have partially written the caller's destination; only success certifies the complete result.

`content_identity(region)` hashes dtype, shape, requested coordinates, facets, and exact sample bits with canonical framing. Block geometry, origin, stride, and allocation do not affect identity. Signed zeros and distinct NaN payloads remain distinct. Snapshot/session identifiers describe provenance; deterministic operations cannot derive sample values or dependencies from identifier spelling.

## Completed Result reuse


A lookup recomputes the source proof against the current Result bindings, replays each saved Need through the ordinary Result coordinator, and compares the ready facts. If the proof and replay match and the cache epoch remains current, the replay Host rebinds the strongly held candidate Result to the current semantic key and source association. The rebound Result gets a new ObjectId and shares the cached physical backing. Cache metadata, proof traversal, hashing and retained payload are charged to their Root/cache limits. Optional proof-work exhaustion, metadata exhaustion, or insufficient retention capacity skips reuse or retention and allows computation to proceed. The completed-result cache uses Result as its semantic input/output object; Value remains typed backing and internal cache storage.

Frozen execution pins its plan and immutable Result bindings; replacing a caller's bindings cannot change the captured inputs.

Borrowed-graph execution continues to enforce graph currentness. The callback must retire before the coordinator resumes actor state.


`maximum_dependency_cache_metadata` defaults to 65536 proof units and is bounded to 1..1048576. `maximum_dependency_cache_work` is a separate per-run traversal limit, defaults to 1048576, and zero disables dependency reuse. `StructuredResultCache` owns and charges this optional quota; cache proof, key construction, replay bookkeeping, and snapshot work use it alongside mandatory Root work. Proof exhaustion skips reuse or retention and permits computation to continue. Disabling optional reuse does not refund Root work already consumed. Deduplication does not erase work already spent traversing, hashing, associating, or normalizing evidence.

## Limits and current entry points

- Native GPU results remain isolated from CPU-exact result keys. A fallback result does not populate native keys, and approximate native results do not replace exact CPU results.
- Native input-copy reuse is separate from completed Result computation reuse. It can reuse transport for demanded Result tensor backing, but it does not make an upstream producer computation cacheable.
- LRU eviction removes the entry from future lookup eligibility. A lookup that already acquired a strong candidate may finish replay and adoption after eviction while its cache remains open and its epoch remains valid. Adoption does not require another LRU-residency proof; an optional promotion lookup affects LRU order only when the same manifest is still resident. Cache closing or an epoch change rejects adoption, while the candidate's independent owner remains valid until the attempt releases it. Frozen executions are in-memory owners, not serialized plans.

current production image operation availability is listed in [Image operations](Image-Operations.md). Snapshot and generic cache behavior is exercised by `test_input_snapshot`, `test_frozen_execution`, `test_generic_result_cache`, and `test_computed_scalar`. Result capture and owner release are covered by `test_result_image_contracts`, `test_global_results`.
