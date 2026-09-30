# Cache model

## Scope & Ownership

Photospider has separate identities and owners for compiled plans, immutable input snapshots, completed CPU/GPU results, native input copies, in-flight shared computations, and disposable disk results. Cache entries retain references to immutable owners; clearing or evicting an entry drops cache eligibility while active readers keep their owners and resource leases alive.

`PlanCacheKey` describes physical plan identity. It excludes input payload and does not validate graph currentness or identify computed results. Result retention is opt-in through `ExecutionContextConfig::result_cache_bytes`, bounded by the context's managed live-byte limit. A zero limit keeps completed results uncached.

## Data Layout & Memory

```cpp
#include "photospider/data/input_snapshot.hpp"

namespace ps {
Result<InputSnapshot> import_snapshot(const InputSnapshotStore& store,
                                      const Value& value) {
  return store.import_value(value);
}
}
```

`InputSnapshotStore` retains immutable rank-1 through rank-8 blocks for Values with the supported built-in dtypes. Generic imports preserve raw valid bit patterns, while supported typed scalar/tensor values are validated. Values carrying image, image-plane, mask, or rank-three-and-higher ColorArray identity require structural planar storage and are rejected by snapshot import. `maximum_blocks` bounds each version's directory; `maximum_bytes` bounds actual retained payload across versions, and directory metadata has a separate bound. Caller-supplied Values and snapshot metadata are outside this byte limit.

Patches require matching dtype, shape, and facets and replace an exact nonempty region. They copy intersecting blocks and preserve old versions. Snapshot access options provide cancellation and a sample bound for import, read, hash, and affected-block copying. A cancelled read may have partially filled caller memory; success alone validates the complete read.

`content_identity(region)` hashes dtype, shape, requested coordinates, facets, and exact sample bits using canonical framing. Block geometry, origin, stride, and allocation do not affect identity. Signed zeros and distinct NaN payloads remain distinct. Snapshot bindings provide regional reads with the Run cancellation token. Snapshot/session identifiers are provenance only; deterministic operations cannot derive sample values or dependencies from the identifier spelling.

## Execution & State

Memory result hits revalidate the resolved descriptor, demanded coverage, output semantic rules, and typed samples, including after a concurrent flight completes. Numeric validation establishes the required floating-point environment and restores the caller's environment. Generic Drop outputs may carry opaque facets; a PreserveInput chain retains that capability while typed declarations still require proven facets. Invalid computed typed values fail with `OperationFailed`.

Regional result keys recursively cover demanded producer regions, operation semantics and parameters, and stable source content. Unrelated graph edits and node identifiers do not invalidate equal content. Whole dependencies remain conservative, and scalar changes invalidate dependent output. An unproven generic input remains executable but disables cross-Run result reuse for its descendants. Only deterministic, side-effect-free operations marked cacheable with a proven implementation qualify.

Small preflight-validated dense offset-zero Values can establish compact content identity when the derived demand covers the complete value; the current bound is 2048 bytes. Snapshot, bounded-scalar, and compact whole-Value sources use distinct key categories. Larger or partial direct inputs remain unproven. Disk result eligibility is narrower and covers supported image and mask Values.

Bounded shared coordinators merge identical in-flight regional computations. CPU work runs in the context callback pool. Each waiting caller observes its own cancellation and currentness. When the last subscriber cancels, the producer drains before the call returns. Producer snapshots own their inputs and registry independently of caller stack and editable graph state.

`FrozenExecution` captures a current plan and immutable Value/snapshot bindings. It remains valid across graph replacement or destruction. Capture copies snapshot handles so later caller-handle replacement cannot change pinned inputs. `for_region` derives a pinned output tile. Custom `RegionalSource` callbacks must be imported before freeze; ordinary plan execution retains stale checks.

The result cache also retains exact dependency observations requested through `DemandHandle` and fragment execution. A manifest holds structural links, transitive Data/Control/Validation footprints, content identities, and fragment keys, but no input, snapshot, or pixel owners. Source bytes, including prior positive and negative control evidence, must match before reuse. Every fragment key includes its actual logical Region; all pieces must remain in the current cache epoch to form a hit. Clearing the cache prevents an old producer from repopulating that epoch.

`ExecutionContextConfig::maximum_dependency_cache_metadata` defaults to 65536 proof units and is bounded to 1..1048576. It counts distinct record owners, row/tag/coordinate storage, and source witnesses within each manifest; a shared owner within one manifest counts once. `maximum_dependency_cache_work` is an optional separate per-Run work budget, defaulting to 1048576; zero disables dependency cache reuse. Proof exhaustion skips reuse or retention and allows computation to continue. Deduplication does not erase already incurred traversal, hashing, association, or normalization work. Snapshot/session identity is provenance, not semantic content identity.

## Algorithms & Math

The bounded disk cache uses canonical SHA-256 keys and an implementation fingerprint for the default operation registry. It stores eligible `Value` outputs with Float32 samples and exactly one valid image or coverage-mask semantic facet. Eligibility checks descriptor, Region, facets, resource bindings, and sample validity. Structural planar execution has a separate path and does not use the Value result or disk cache. Each disk record validates dtype, shape, Region, facet keys and payloads, byte count, checksum, and result key before publication. Reads size allocations from the validated plan, not file-provided lengths. A malformed, truncated, mismatched, or unsupported record is treated as a disposable miss.

Writes use a temporary file and rename only after the complete record is written. The cache makes no durable recovery guarantee. Queue pressure or write failure skips retention without failing the computed result. Pending writes retain accounted source buffers and can be discarded under computation pressure. `flush_disk_cache()` waits for queued writes; `clear_disk_cache()` removes entries and invalidates pending write epochs. The configured directory is exclusively locked by one context; unrelated files are ignored.

The public API and focused behavior checks are in `include/photospider/data/input_snapshot.hpp`, `tests/unit/test_input_snapshot.cpp`, and `tests/integration/test_frozen_execution.cpp`. The public planar execution workflow is covered by `tests/integration/test_planar_image_workflow.cpp`; it exercises structural image bindings and does not imply snapshot or result-cache support for planar pages.

## Limitations & Non-Goals

- Disk caching requires explicit configuration and positive in-memory result-cache capacity. It is disposable local storage, not a durable result store.
- Persistent implementation identity is available only for the default registry. Custom and C-module registries can use process-local caching but do not receive a persistent implementation identity.
- Disk reads and writes apply only when the compiled plan's `execution_mode` is `CpuExact`. Selecting a native GPU plan does not gain disk-cache eligibility when backend selection later falls back to CPU; native GPU results remain isolated from CPU-exact keys.
- A fallback result and its descendants do not populate native result keys. Approximate native results cannot replace exact CPU results.
- Native input-copy reuse hashes demanded logical bytes, descriptor, facets, Region, and device identity. It can avoid an upload for immutable ordinary Values; it does not make arbitrary `RegionalSource` computation cacheable.
- Cache clear and eviction remove eligibility without invalidating active borrowed data. Frozen executions are in-memory owners; there is no serialized frozen-plan reader.
