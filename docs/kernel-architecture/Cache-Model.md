# Cache model

## Scope and ownership

The kernel keeps separate identities for compiled plans, immutable input snapshots, completed structured Result reuse, shared in-flight Result work, and native backend input-copy caches. Cache membership grants reuse eligibility; active Results, snapshots, typed backing Values, and read windows retain their own owners and resource leases after an entry is cleared or evicted.

`PlanCacheKey` identifies a physical plan. It excludes input payload and does not prove graph currentness or identify a computed output. Completed Result reuse is weakly indexed: the cache can locate a still-live Result, but the cache entry alone does not keep its payload alive. A strong `ResultRef`, captured publication, or active waiter owns that Result. `ResultRef::capture()` snapshots the certified descriptor, field/image relations, descriptor basis, and dependency bundle at one revision. The Result’s ordered source association remains a separate monotone owner relation on the shared Result object and can grow as inputs are consumed.

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

## Completed Result reuse and shared work

The completed-result cache stores sealed structured Results for cacheable operations whose selected-input producer closure is pure and whose backend matches the compiled plan. Its key includes the compiled operation template, selected output, backend, tensor slot, and exact requested tensor footprint Q; it does not use node IDs or runtime input ObjectIds. Before retention, the host records the Result dependency bundle's source observations and hashes the observed descriptors or field/tensor bytes from the current Result bindings. It also records the callback's Result Needs and the supplied descriptor/coverage facts required to replay them. Observed backend fallback, quality outcomes, unproven source support, `photospider.path_set`, and ineligible operation closures are not retained.

A lookup recomputes the source proof against the current Result bindings, replays each saved Need through the ordinary Result coordinator, and compares the ready facts. If the proof and replay match and the LRU entry remains eligible, the host rebinds the cached Result to the current semantic key and source association. The rebound Result gets a new ObjectId and shares the cached physical backing. Cache metadata, proof traversal, hashing and retained payload are charged to their Root/cache limits. Optional proof-work exhaustion, metadata exhaustion, or insufficient retention capacity skips reuse or retention and allows computation to proceed. The completed-result cache uses Result as its semantic input/output object; Value remains typed backing and internal cache storage.

Frozen execution pins its plan and immutable Result bindings; replacing a caller's bindings cannot change the captured inputs. Same-key active Result requests may share a producer independently of completed-result retention. Each waiter keeps its own cancellation and currentness state. A waiter can consume a captured publication while the producer or another waiter continues. Cancelling one waiter does not cancel work still needed by another; when the final waiter leaves, admitted producer work drains before its owners retire. Completed lookup metadata uses weak Result references, so releasing the last strong Result owner releases its image pages, fields, relations, and retained input associations.

When a frozen-plan caller is cancelled or its Demand binding generation becomes stale while its structured Result producer callback is pending and a healthy peer still needs the producer, `SharedResults` can retain the coordinator and let that peer drive it from the thread already waiting for the Result. The originating caller still receives `Cancelled` or `Stale` at its final check; the frozen peer keeps its captured plan and bindings. Borrowed-graph execution continues to enforce graph currentness. The callback must retire before the coordinator resumes actor state. If adoption fails or the peer leaves before adoption completes, the caller withdraws the temporary registry owner and drains synchronously; the same run does not retry adoption. A borrowed-plan call or a call with no live peer also drains synchronously. Context shutdown first cancels shared work under the registry lock, then releases that lock and drains every parked coordinator; this path does not create a worker thread or change synchronous cache-replay behavior.

Dependency cache manifests retain immutable relation evidence, source observations, content identities, and fragment keys. They do not own input or image payloads. Reuse compares source bytes over recorded support, including consumed Control observations. A changed consumed Control can discover different Data support on the next request; unrelated or unconsumed controls do not add support. Result and dependency maps/vectors retain their root-accounted metadata owner after return, so copies remain charged while callers hold them.

`maximum_dependency_cache_metadata` defaults to 65536 proof units and is bounded to 1..1048576. `maximum_dependency_cache_work` is a separate per-run traversal limit, defaults to 1048576, and zero disables dependency reuse. Proof exhaustion skips reuse or retention and permits computation to continue. Deduplication does not erase work already spent traversing, hashing, associating, or normalizing evidence.

## Limits and current entry points

- Native GPU results remain isolated from CPU-exact result keys. A fallback result does not populate native keys, and approximate native results do not replace exact CPU results.
- Native input-copy reuse is separate from completed Result computation reuse. It can reuse transport for demanded Result tensor backing, but it does not make an upstream producer computation cacheable.
- LRU eviction removes the entry from future lookup eligibility. A lookup that already holds a strong candidate may finish its replay after eviction while the cache epoch remains unchanged and Root capacity still permits use of the candidate's retained Result. A new lookup cannot verify that evicted entry. `clear`, cache closing, or an epoch change rejects pending candidate reuse, even when an active lookup still strongly owns the cached Result; that independent Result owner remains valid. Frozen executions are in-memory owners, not serialized plans.

The public Result image entry point is [`examples/unified_result_workflow`](../../examples/unified_result_workflow/README.md); current production image operation availability is listed in [Image operations](Image-Operations.md). Snapshot and generic cache behavior is exercised by `test_input_snapshot`, `test_frozen_execution`, `test_generic_result_cache`, `test_shared_results`, and `test_computed_scalar_sharing`. Result capture and owner release are covered by `test_result_image_contracts`, `test_global_results`, and `test_shared_results`.
