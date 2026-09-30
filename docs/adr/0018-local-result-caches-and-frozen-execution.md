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
                           memory result cache
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

Snapshots contain supported rank-1..8 `Value` inputs and preserve their sample bits and facets. Each snapshot is immutable; `patch` accepts a matching descriptor and facets plus a nonempty replacement Region, copies intersecting blocks, and leaves every earlier version readable. The store enforces one aggregate payload-byte limit across retained versions. Metadata and caller-owned Values are outside that limit. Snapshot import rejects image facets, rank-three-or-higher color arrays, `ImagePlane` and typed `Mask` Values because these require structured image storage. Image data is represented by typed image slots in structured Results; a Result image slot owns its `PlanarImage` backing.

`content_identity` hashes canonical metadata and the requested exact sample bits. Shape, dtype, coordinates and facets participate; allocation address, layout and block geometry do not. Read and identity operations accept sample bounds and cooperative cancellation. A cancelled read may have partially filled the caller's destination, which the caller must discard.

`freeze` captures a current matching plan, immutable bindings and the operation registry owner. A custom `RegionalSource` must first be imported into a kernel snapshot. Workflow input bindings supplied to freeze are immutable Values, kernel snapshots, or immutable `ResultRef` owners for declarations with a Result schema. Preflight checks that a Result binding matches the declared schema and has a valid final descriptor. Execution admission checks root ownership when the context uses managed resources, and demand reads remain bounded by the declared domain and published support. Capture invokes no operation callback and rechecks graph currentness before returning. Frozen execution uses the captured input versions after graph replacement or destruction; ordinary execution retains its normal currentness checks. Each execute call supplies independent cancellation, and owners remain alive until admitted callbacks retire. A Result input binding retains its immutable `ResultRef` owner. Callers capture a Result with `ResultRef::capture()` and read image samples through `ResultRef::read_image()` using the captured descriptor.

Completed-result retention is optional. Its capacity is a sublimit of `maximum_live_bytes`; zero disables retention while exact in-flight demand sharing remains available. Keys cover the local operation contract and implementation, exact demanded input content and metadata, output Region and relevant backend identity. Compiler graph revision and unrelated branches do not enter a local result key. A dirty hint can narrow work but cannot establish reuse without content identity. Reuse requires every participating operation to be deterministic, side-effect-free and cacheable, with proven input dependencies.

The coordinator lets identical in-flight requests in one context share a producer. Each waiter has independent cancellation. Cancelling one waiter leaves other live waiters attached; when the last waiter leaves, the coordinator requests producer cancellation. The producer owns its frozen inputs and registry until admitted callbacks retire. It does not block a worker by waiting for another callback in the same pool. Only validated successful immutable results enter completed caches.



## 4. Non-goals and explicit boundaries

- Snapshots cover immutable input Values and their content; they do not serialize an execution plan or workflow document.
- A frozen execution pins one in-memory plan and its inputs; it does not make later graph edits current for ordinary execution. Workflow input bindings must be immutable Values, kernel snapshots, or a schema-matching immutable Result owner.
- Result caches are disposable accelerators. They do not provide transactional recovery, durable commits, request history or authoritative artifacts.
- The kernel does not own preview queues, edit coalescing, user-visible freshness policy or publication decisions.
- Unproven mutable sources may execute, but their dependent results cannot be reused across runs.

## 5. Consequences

Retaining old snapshots and returned Values retains their backing blocks and leases. Patches consume additional budget for changed blocks while older versions remain referenced. A snapshot import or patch that exceeds the store budget fails with `ResourceExhausted`; callers can release obsolete versions and retry.

The cache reclaims idle entries before resource admission. Eviction and cache clearing remove reuse eligibility but do not invalidate Values already held by callers. Cache admission failure skips retention; if the working set itself exceeds the context's controlled-buffer capacity, execution fails with `ResourceExhausted`. The caller must release unneeded results to make their owned capacity available.

Cancellation is cooperative. Submitted work retires before its owners are released, and a producer continues while any subscriber still needs it.
