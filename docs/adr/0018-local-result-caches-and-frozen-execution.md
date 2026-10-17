# ADR 0018: Freeze Inputs and Reuse Exact Local Results

- Status: Accepted
- Reader mirror: [Chinese](zh/0018-local-result-caches-and-frozen-execution.zh.md)
- Current boundary: in 0.36.0 the snapshot facets and the import rejection of image, color-array, `ImagePlane` and `Mask` Values of the original decision are superseded. Snapshots validate structure only; run data is bound as Result static Tensor members.

## 1. Core summary (TL;DR)

Mutable workflow graphs and edited inputs need stable execution inputs, while tiled work benefits from reusing tiles whose recorded dependencies have not changed. The kernel owns immutable input snapshots, frozen execution bundles and bounded disposable result caches. Content identity and tile dependency records permit reuse without making cache state authoritative.

## 2. Mental model and intuition

`InputSnapshotStore` works like a versioned block store: a patch copies the blocks it intersects and shares the rest with earlier versions. A frozen execution pins one compiled plan, its registry and its immutable bindings, so later graph edits do not change that run.

```text
Graph + bindings --freeze--> Frozen plan and input owners
                                  |
                     requested member tile
                           /             \
                     cache hit         cache miss
                       |                  |
                       |         compute in this Run
                       |                  |
                       +---------- validated immutable result
                                      |
                           memory result cache
```

An `ExecutionContext` owns worker resources and its cache entries. A result may outlive the context when a returned `Tensor` still owns the result storage and its budget lease. Application code owns preview policy, event queues, publication arbitration and presentation.

## 3. Formal contracts and APIs

```cpp
struct InputSnapshotStoreConfig {
  std::uint64_t maximum_bytes = 256U * 1024U * 1024U;
  std::uint32_t block_size = 128;
  std::uint64_t maximum_blocks = 65536;
};

class InputSnapshotStore {
 public:
  Result<InputSnapshot> import_tensor(const Tensor&, const SnapshotAccessOptions& = {}) const;
  Result<InputSnapshot> patch(const InputSnapshot&, const Tensor& replacement,
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

Snapshots contain complete rank-1..8 `Tensor` inputs of any built-in element type and preserve their exact sample bits. Import requires the Tensor's coverage to cover its full shape and validates only this structure, so a Tensor that holds image, color or mask samples is imported like any other tensor of the same element type and shape. Each snapshot is immutable; `patch` accepts a replacement with the same element type and shape plus a nonempty contained Footprint, copies intersecting blocks, and leaves every earlier version readable. The store enforces one aggregate payload-byte limit across retained versions. Metadata and caller-owned Tensors are outside that limit. Run data supplied to a workflow, including images, is bound as a Result whose static Tensor members hold the samples.

`content_identity` hashes a versioned identity prefix, the element type, shape, requested region coordinates and the requested exact sample bits. Allocation address, layout and block geometry do not participate. Read and identity operations accept sample bounds and cooperative cancellation. A cancelled read may have partially filled the caller's destination, which the caller must discard.

`freeze` captures a current matching plan, immutable bindings and the operation registry owner. Each workflow input binding is an `ExecutionBinding` that names one declaration and supplies an immutable `ResultRef`. Preflight checks that a Result binding matches the declared schema and has a valid final descriptor. Execution admission checks root ownership when the context uses managed resources, and demand reads remain bounded by the declared domain and published support. Capture invokes no operation callback and rechecks graph currentness before returning. Frozen execution uses the captured input versions after graph replacement or destruction; ordinary execution retains its normal currentness checks. Each execute call supplies independent cancellation, and owners remain alive until admitted callbacks retire. A Result input binding retains its immutable `ResultRef` owner. Callers capture a Result with `ResultRef::capture()` and read tensor samples through `ResultRef::read_tensor()` or `ResultRef::acquire_tensor()` using the captured descriptor.

Completed-result retention is optional. Its capacity is a sublimit of `maximum_live_bytes`; zero disables retention. The reuse unit is one member tile. Its key combines the step's physical identity (operation contract and implementation, tile extent, resolved output schema, and relevant backend and resource identities), the unit index and the identities of the producer units recorded for it, as defined by [ADR 0023](0023-track-dependencies-between-fixed-tiles.md); workflow-input identities are computed over logical content. Compiler graph revision and unrelated branches do not enter a local result key. A dirty hint can narrow work but cannot establish reuse without content identity. Reuse requires every participating operation to be deterministic, side-effect-free and cacheable.

Each Run computes its own cache misses; identical requests in concurrent Runs are not merged. A Run's producers own their frozen inputs and registry until admitted callbacks retire. It does not block a worker by waiting for another callback in the same pool. Only validated successful immutable results enter completed caches.

## 4. Non-goals and explicit boundaries

- Snapshots cover immutable input Tensors and their content; they do not serialize an execution plan or workflow document.
- A frozen execution pins one in-memory plan and its inputs; it does not make later graph edits current for ordinary execution. Workflow input bindings must be schema-matching immutable Result owners.
- Result caches are disposable accelerators. They do not provide transactional recovery, durable commits, request history or authoritative artifacts.
- The kernel does not own preview queues, edit coalescing, user-visible freshness policy or publication decisions.
- Unproven mutable sources may execute, but their dependent results cannot be reused across runs.

## 5. Consequences

Retaining old snapshots and returned Tensors retains their backing blocks and leases. Patches consume additional budget for changed blocks while older versions remain referenced. A snapshot import or patch that exceeds the store budget fails with `ResourceExhausted`; callers can release obsolete versions and retry.

The cache reclaims idle entries before resource admission. Eviction and cache clearing remove reuse eligibility but do not invalidate Tensors already held by callers. Cache admission failure skips retention; if the working set itself exceeds the context's controlled-buffer capacity, execution fails with `ResourceExhausted`. The caller must release unneeded results to make their owned capacity available.

Cancellation is cooperative. Submitted work retires before its owners are released, and cancelling one Run does not affect another Run.

Run-shared stages guarantee one producer and retain success/failure until Run end independently of optional caching. They never retain active continuations across Runs. Checkpoint/block/completed reuse has its own eligibility and cannot widen authorization; see [Global Results](../kernel-architecture/Global-Results.md).
