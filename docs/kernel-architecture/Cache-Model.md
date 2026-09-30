# Cache model

## Scope and ownership

The kernel keeps separate identities for compiled plans, immutable input snapshots, ordinary Value results, shared in-flight Result work, and disposable disk records. Cache membership grants reuse eligibility; active Values, Results, snapshots, and read windows retain their own owners and resource leases after an entry is cleared or evicted.

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

## Value result reuse and shared work

The Value result cache revalidates descriptors, demanded coverage, output semantic rules, and typed samples before returning a hit. Regional keys cover demanded producer regions, operation semantics and parameters, and stable source content. Unrelated graph edits and node IDs do not invalidate equal content. Whole dependencies remain conservative. An unproven generic input remains executable but disables cross-run reuse for dependent descendants. Only deterministic, side-effect-free, cacheable operations with proven implementation identity qualify.

Small preflight-validated dense offset-zero Values can establish compact content identity when the derived demand covers the full Value and stays within the current 2048-byte bound. Snapshot, bounded-scalar, and compact whole-Value sources use distinct key categories. Larger or partial direct inputs remain unproven. Frozen execution pins its plan and immutable input bindings; replacing a caller's snapshot handle cannot change the frozen input.

Shared Result execution captures one immutable input bundle and selected output query. Identical in-flight requests may share a producer. Each waiter keeps its own cancellation and currentness state. A waiter can consume a captured publication while the producer or another waiter continues. Cancelling one waiter does not cancel work still needed by another; when the final waiter leaves, admitted producer work drains before its owners retire. Completed lookup metadata uses weak Result references, so releasing the last strong Result owner releases its image pages, fields, relations, and retained input associations.

Dependency cache manifests retain immutable relation evidence, source observations, content identities, and fragment keys. They do not own input or image payloads. Reuse compares source bytes over recorded support, including consumed Control observations. A changed consumed Control can discover different Data support on the next request; unrelated or unconsumed controls do not add support. Result and dependency maps/vectors retain their root-accounted metadata owner after return, so copies remain charged while callers hold them.

`maximum_dependency_cache_metadata` defaults to 65536 proof units and is bounded to 1..1048576. `maximum_dependency_cache_work` is a separate per-run traversal limit, defaults to 1048576, and zero disables dependency reuse. Proof exhaustion skips reuse or retention and permits computation to continue. Deduplication does not erase work already spent traversing, hashing, associating, or normalizing evidence.

## Disk cache

The disk cache stores eligible ordinary Value records using canonical SHA-256 keys and a default-registry implementation fingerprint. It validates the descriptor, Region, facets, payload length, checksum, and result key before publication. Allocation size comes from the validated plan, not file-provided lengths. A malformed, truncated, mismatched, or unsupported record is a disposable miss.

Writes use a temporary file and rename after the complete record is written. The cache is not a durable result store. Queue pressure or write failure skips retention without failing the computed result. `flush_disk_cache()` waits for queued writes; `clear_disk_cache()` removes entries and invalidates pending write epochs. One context exclusively locks its configured directory; unrelated files are ignored. Disk eligibility is limited to the supported CPU-exact ordinary Value contract; structured image Results use Result ownership and identity.

## Limits and current entry points

- Disk caching requires explicit configuration and positive in-memory Value-cache capacity.
- Persistent implementation identity is available for the default registry. Custom and C-module registries can use process-local caching without persistent implementation identity.
- Native GPU results remain isolated from CPU-exact result keys. A fallback result does not populate native keys, and approximate native results do not replace exact CPU results.
- Native input-copy reuse hashes demanded logical bytes, descriptor, facets, Region, and device identity. It can avoid uploading immutable Values but does not make arbitrary `RegionalSource` computation cacheable.
- Clearing or evicting cache entries removes reuse eligibility without invalidating active owners. Frozen executions are in-memory owners, not serialized plans.

The public Result image entry point is [`examples/unified_result_workflow`](../../examples/unified_result_workflow/README.md); current production image operation availability is listed in [Image operations](Image-Operations.md). Snapshot and generic cache behavior is exercised by `test_input_snapshot`, `test_frozen_execution`, `test_generic_result_cache`, and `test_shared_results`. Result capture, owner release, and image-cache behavior are covered by `test_result_image_contracts`, `test_global_results`, and `test_shared_results`.
