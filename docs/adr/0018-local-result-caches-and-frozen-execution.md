# ADR 0018: Freeze Inputs and Reuse Exact Local Results

- Status: Accepted
- Reader mirror: [Chinese](zh/0018-local-result-caches-and-frozen-execution.zh.md)

## 1. Core summary (TL;DR)

Mutable workflow graphs and edited inputs need stable execution inputs, while regional work benefits from reusing results whose exact dependencies have not changed. The kernel owns immutable input snapshots, frozen execution bundles and bounded disposable result caches. Exact content and dependency identity permit reuse without making cache state authoritative.

## 2. Mental model and intuition

`InputSnapshotStore` works like a versioned block store: a patch copies the blocks it intersects and shares the rest with earlier versions. A frozen execution pins one compiled plan, its registry and its immutable bindings, so later graph edits do not change that run.

```text
Graph + bindings --freeze--> Frozen plan and input owners
                                  |
                         requested local result
                           /             \
                  exact cache hit      cache miss
                       |                  |
                       |             producer flight
                       |             /            \
                       |       subscriber A    subscriber B
                       |             \            /
                       +---------- validated immutable result
                                      |
                           memory cache / optional disk cache
```

An `ExecutionContext` owns worker resources, in-flight coordination and its cache entries. A result may outlive the context when a returned `Value` still owns the result storage and its budget lease. Application code owns preview policy, event queues, publication arbitration and presentation.

## 3. Formal contracts and APIs

```cpp
struct InputSnapshotStoreConfig {
  std::uint64_t maximum_bytes = 256U * 1024U * 1024U;
  std::uint32_t block_size = 128;
  std::uint64_t maximum_blocks = 65536;
};

class InputSnapshotStore {
 public:
  Result<InputSnapshot> import_value(const Value&, const SnapshotAccessOptions& = {}) const;
  Result<InputSnapshot> patch(const InputSnapshot&, const Value& replacement,
                              const SnapshotAccessOptions& = {}) const;
  std::uint64_t live_bytes() const;
};

struct ExecutionContextConfig {
  std::uint32_t cpu_workers = 0;
  bool gpu_enabled = false;
  std::uint32_t maximum_queued_tasks = 1024;
  std::uint64_t maximum_live_bytes = 256U * 1024U * 1024U;
  std::uint64_t result_cache_bytes = 0;
  std::optional<DiskCacheConfig> disk_cache = {};
};

class ExecutionContext {
 public:
  Result<FrozenExecution> freeze(const ExecutionPlan&,
                                 ExecutionBindings = {}) const;
  Result<ExecutionResult> execute(const FrozenExecution&,
                                  const CancellationToken& = {},
                                  const ExecutionOptions& = {});
};
```

The excerpt omits unrelated members of `ExecutionContext`.

Snapshots contain supported rank-1..8 `Value` inputs and preserve their sample bits and facets. Each snapshot is immutable; `patch` accepts a matching descriptor and facets plus a nonempty replacement Region, copies intersecting blocks, and leaves every earlier version readable. The store enforces one aggregate payload-byte limit across retained versions. Metadata and caller-owned Values are outside that limit. In the current implementation, snapshot import rejects `photospider.image`, rank-three-or-higher `photospider.color-array`, `ImagePlane` and typed `Mask` values because these require structural planar storage; image workflows use the `PlanarImage` binding path instead.

`content_identity` hashes canonical metadata and the requested exact sample bits. Shape, dtype, coordinates and facets participate; allocation address, layout and block geometry do not. Read and identity operations accept sample bounds and cooperative cancellation. A cancelled read may have partially filled the caller's destination, which the caller must discard.

`freeze` captures a current matching plan, immutable bindings and the operation registry owner. A custom `RegionalSource` must first be imported into a kernel snapshot. Capture invokes no operation callback and rechecks graph currentness before returning. Frozen execution uses the captured input versions after graph replacement or destruction; ordinary execution retains its normal currentness checks. Each execute call supplies independent cancellation, and owners remain alive until admitted callbacks retire. The current capture API rejects plans that require structural planar image capture; it does not freeze the `PlanarImage` binding path.

Completed-result retention is optional. Its capacity is a sublimit of `maximum_live_bytes`; zero disables retention while exact in-flight demand sharing remains available. Keys cover the local operation contract and implementation, exact demanded input content and metadata, output Region and relevant backend identity. Compiler graph revision and unrelated branches do not enter a local result key. A dirty hint can narrow work but cannot establish reuse without content identity. Reuse requires every participating operation to be deterministic, side-effect-free and cacheable, with proven input dependencies.

The coordinator lets identical in-flight requests in one context share a producer. Each waiter has independent cancellation. Cancelling one waiter leaves other live waiters attached; when the last waiter leaves, the coordinator requests producer cancellation. The producer owns its frozen inputs and registry until admitted callbacks retire. It does not block a worker by waiting for another callback in the same pool. Only validated successful immutable results enter completed caches.

An optional disk cache requires an explicitly selected local directory, a positive in-memory result-cache capacity and the maintained built-in operation registry identity. Custom operation registries have no persistent-cache identity, so disk lookups miss and writes are ineligible. The cache stores only CPU-exact Float32 values with image-v2 or canonical coverage-mask metadata, under bounded byte, entry and queued-write limits. The kernel exclusively locks the selected directory while the cache is alive. Startup removes abandoned temporary files and indexes bounded entries; a disk hit is checked against the expected key, descriptor, facets, semantic samples and checksum before use. Unsupported or corrupt entries are treated as misses. Disk data never restores workflow state or grants result authority; failed or pressured asynchronous writes are dropped. GPU-mode runs do not read or write disk entries.

## 4. Non-goals and explicit boundaries

- Snapshots cover immutable input Values and their content; they do not serialize an execution plan or workflow document.
- A frozen execution pins one in-memory plan and its inputs; it does not make later graph edits current for ordinary execution. `freeze` currently rejects plans that require structural planar image capture.
- Result caches are disposable accelerators. They do not provide transactional recovery, durable commits, request history or authoritative artifacts.
- The kernel does not own preview queues, edit coalescing, user-visible freshness policy or publication decisions.
- Unproven mutable sources may execute, but their dependent results cannot be reused across runs.
- Disk persistence is restricted to the formats accepted by the current disk-cache implementation. Cache files are not a plugin, interchange or backup format.

## 5. Consequences

Retaining old snapshots and returned Values retains their backing blocks and leases. Patches consume additional budget for changed blocks while older versions remain referenced. A snapshot import or patch that exceeds the store budget fails with `ResourceExhausted`; callers can release obsolete versions and retry.

The cache reclaims idle entries before resource admission. Eviction and cache clearing remove reuse eligibility but do not invalidate Values already held by callers. Cache admission failure skips retention; if the working set itself exceeds the context's controlled-buffer capacity, execution fails with `ResourceExhausted`. The caller must release unneeded results to make their owned capacity available.

Cancellation is cooperative. Submitted work retires before its owners are released, and a producer continues while any subscriber still needs it. Disk cache writes may be dropped under I/O failure or queue pressure, so callers must treat a later miss as normal. `flush_disk_cache` is explicit; result publication does not wait for disk writes.
