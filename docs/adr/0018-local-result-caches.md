# ADR 0018: Retain Immutable Inputs and Reuse Exact Local Results

- Status: Accepted
- Reader mirror: [Chinese](zh/0018-local-result-caches.zh.md)
- Current boundary: in 0.36.0 the snapshot facets and the import rejection of image, color-array, `ImagePlane` and `Mask` Values of the original decision are superseded. Snapshots validate structure only; run data is bound as Result static Tensor members.

## Current implementation: content identity and call activation

In the static Plan implementation, a completed-result key includes the operation,
parameters, fixed schemas, resources, output member/tile, backend/mode and fixed
read ranges plus input sample content. Physical node, step and Plan identifiers
are excluded; compilation digests still include program IDs. Reuse reconstructs
evidence for the current Plan instead of replaying old physical dependency
records. This specifies the content identity behind the physical-key terminology
in the original decision below.

Pure deterministic closure and completed-result cacheability are separate:
block/checkpoint services can be eligible when completed-result retention is
disabled. Checkpoints match content identity without requiring the old physical
TileUnit; current grants and evidence remain authoritative. Shared-stage and
block-parameter paths conservatively bypass optional reuse as described in
[Cache model](../kernel-architecture/Cache-Model.md). GPU block retention requires
native dispatch during compute; `native_retained_bytes` includes retained GPU
outputs as well as block state. Fallback-tainted results bypass reuse.

Shared computation is unique within one call activation. Entering a block again
clears that block's values and shared-stage state, so a loop recomputes the next
activation rather than retaining one producer result for the whole Run. Compiler
artifacts persist compiled stages only; per-call bindings and disposable caches
remain in-memory state.

## 1. Core summary (TL;DR)

Mutable workflow graphs and edited inputs need stable execution inputs, while tiled work benefits from reusing tiles whose recorded dependencies have not changed. The kernel owns immutable input snapshots, per-call Result bindings and bounded disposable result caches. Content identity and tile dependency records permit reuse without making cache state authoritative.

## 2. Mental model and intuition

`InputSnapshotStore` works like a versioned block store: a patch copies the blocks it intersects and shares the rest with earlier versions. Each Run uses a current compiled plan and retains its immutable bindings until callbacks retire.

```text
Current plan + bindings --> Run and input owners
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
  Result<ExecutionResult> execute(const ExecutionPlan&,
                                  ExecutionBindings = {},
                                  const CancellationToken& = {},
                                  const ExecutionOptions& = {});
  Result<DemandResult> execute_fragments(const ExecutionPlan&,
                                         ExecutionBindings,
                                         const DemandQuery&,
                                         const CancellationToken& = {},
                                         const ExecutionOptions& = {});
};
```

The excerpt omits unrelated members of `ExecutionContext`.

Snapshots contain complete rank-1..8 `Tensor` inputs of any built-in element type and preserve their exact sample bits. Import requires the Tensor's coverage to cover its full shape and validates only this structure, so a Tensor that holds image, color or mask samples is imported like any other tensor of the same element type and shape. Each snapshot is immutable; `patch` accepts a replacement with the same element type and shape plus a nonempty contained Footprint, copies intersecting blocks, and leaves every earlier version readable. The store enforces one aggregate payload-byte limit across retained versions. Metadata and caller-owned Tensors are outside that limit. Run data supplied to a workflow, including images, is bound as a Result whose static Tensor members hold the samples.

`content_identity` hashes a versioned identity prefix, the element type, shape, requested region coordinates and the requested exact sample bits. Allocation address, layout and block geometry do not participate. Read and identity operations accept sample bounds and cooperative cancellation. A cancelled read may have partially filled the caller's destination, which the caller must discard.

Each execution validates a current plan and copies its immutable Result bindings. An `ExecutionBinding` names one workflow input declaration and supplies a schema-matching `ResultRef`. Preflight validates descriptors and schemas; execution admission checks resource-root ownership. Graph replacement or destruction makes a borrowed-graph plan stale, including plans retained by a `DemandHandle`. A demand request also checks its binding generation. Each call supplies independent cancellation and retains input owners until admitted callbacks retire. Callers capture a Result with `ResultRef::capture()` and read samples through `ResultRef::read_tensor()` or `ResultRef::acquire_tensor()` using the captured descriptor.

Completed-result retention is optional. Its capacity is a sublimit of `maximum_live_bytes`; zero disables retention. The reuse unit is one member tile. Its key combines the step's physical identity (operation contract and implementation, tile extent, resolved output schema, and relevant backend and resource identities), the unit index and the identities of the producer units recorded for it, as defined by [ADR 0023](0023-track-dependencies-between-fixed-tiles.md); workflow-input identities are computed over logical content. Compiler graph revision and unrelated branches do not enter a local result key. A dirty hint can narrow work but cannot establish reuse without content identity. Reuse requires every participating operation to be deterministic, side-effect-free and cacheable.

Each Run computes its own cache misses; identical requests in concurrent Runs are not merged. A Run's producers own their immutable inputs and registry until admitted callbacks retire. It does not block a worker by waiting for another callback in the same pool. Only validated successful immutable results enter completed caches.

## 4. Non-goals and explicit boundaries

- Snapshots cover immutable input Tensors and their content; they do not serialize an execution plan or workflow document.
- Workflow input bindings must be schema-matching immutable Result owners. A borrowed-graph plan requires its graph revision to remain current throughout execution.
- Result caches are disposable accelerators. They do not provide transactional recovery, durable commits, request history or authoritative artifacts.
- The kernel does not own preview queues, edit coalescing, user-visible freshness policy or publication decisions.
- Unproven mutable sources may execute, but their dependent results cannot be reused across runs.

## 5. Consequences

Retaining old snapshots and returned Tensors retains their backing blocks and leases. Patches consume additional budget for changed blocks while older versions remain referenced. A snapshot import or patch that exceeds the store budget fails with `ResourceExhausted`; callers can release obsolete versions and retry.

The cache reclaims idle entries before resource admission. Eviction and cache clearing remove reuse eligibility but do not invalidate Tensors already held by callers. Cache admission failure skips retention; if the working set itself exceeds the context's controlled-buffer capacity, execution fails with `ResourceExhausted`. The caller must release unneeded results to make their owned capacity available.

Cancellation is cooperative. Submitted work retires before its owners are released, and cancelling one Run does not affect another Run.

Run-shared stages guarantee one producer and retain success/failure until Run end independently of optional caching. They never retain active continuations across Runs. Checkpoint/block/completed reuse has its own eligibility and cannot widen authorization; see [Global Results](../kernel-architecture/Global-Results.md).
