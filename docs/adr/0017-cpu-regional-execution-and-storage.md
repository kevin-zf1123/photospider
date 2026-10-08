# ADR 0017: Execute Regional Work with Bounded CPU Storage

- Status: Accepted

## 1. Core Summary (TL;DR)

The planner propagates requested output regions through operation traits, and the execution context schedules the resulting work with bounded CPU workers and memory accounting. Dense `Value` storage remains immutable and region-addressable; structural images use `PlanarImage` owners and publish only completed writes. Callers can collect requested outputs or synchronously consume ordered output tiles.

## 2. Mental Model & Intuition

```text
requested output region
          |
          v
planner propagates demands and partitions splittable axes
          |
          v
bounded admission -> CPU workers -> operation callbacks
       |                   / \
queue or budget      successful output    failure/cancel/stale
limit rejects               |                 |
                          v                 v
                     publish result    stop admissions
                          |            drain admitted work
                    collect / sink          |
                                      return failure
```

The planner works backward from each requested output and records input demand. The execution context admits work only when queue and allocation limits permit it. CPU workers invoke callbacks over the planned regions, publish immutable results, and release storage when its final owner retires. A streaming sink applies synchronous backpressure because the executor returns from one sink call before delivering the next tile.

## 3. Formal Contracts & APIs

```cpp
struct RegionDimension { std::uint64_t offset; std::uint64_t extent; };
class Region;
class Value;
class PlanarImage;
struct ExecutionPlan;
struct ExecutionBindings;
struct ExecutionOptions;
struct ExecutionContextConfig;

using ExecutionSink =
    std::function<Status(const std::string&, ValueView)>;

class ExecutionContext {
 public:
  [[nodiscard]] Result<ExecutionResult> execute(
      const ExecutionPlan& plan, ExecutionBindings bindings = {},
      const CancellationToken& cancellation = CancellationToken(),
      const ExecutionOptions& options = {});
  [[nodiscard]] Result<ExecutionDiagnostics> execute_stream(
      const ExecutionPlan& plan, ExecutionBindings bindings,
      const ExecutionSink& sink,
      const CancellationToken& cancellation = CancellationToken(),
      const ExecutionOptions& options = {});
};
```

These declarations reproduce the current public member signatures; enclosing headers and unrelated methods are omitted.

`Region` stores half-open logical intervals in descriptor axis order. Coordinates describe samples, not byte offsets. The planner validates requests against output descriptors and propagates demand according to operation traits such as Whole, elementwise, or halo behavior. It clips halo demand at logical image bounds. Tiling partitions only axes authorized by the operation and image contract; a tile retains the complete channel group when the channel axis is atomic. Collected outputs contain the exact planned output coverage, while the descriptor continues to describe the full logical value.

`Value` describes a logical descriptor, valid region, strided byte layout, facets, resource owners, and immutable `CpuStorage`. `Value::view(region)` returns an owning `Value` that shares the storage owner without copying. `ValueView` instead borrows a `Value` and owns no storage; its referenced value must outlive the view. `MutableBuffer` is exclusive and move-only; `freeze()` transfers its storage into immutable ownership. A published allocation remains alive while any owning `Value` retains it. Retiled and regional operations copy only the requested coverage into host-managed output storage.

`PlanarImageLayout` defines height, width, optional channel axes, component groups, row pitch, and continuous or tiled physical order. Logical axis order does not imply interleaved physical storage. `PlanarImage` owns immutable-published samples; writers use unpublished write windows and commit completed regions. `PlanarPageBudget` accounts backed image pages independently from the maximum virtual reservation. Page owners and their accounting leases may outlive the execution context. The ordinary `execute_stream` entry points accept Value-based regional output and return `TypeMismatch` for plans that require structural planar output; planar execution uses the structural image result path.

Operation plugins use the standalone Result C ABI 2 declared in [`result_operation_plugin_api.h`](../../include/photospider/plugin/result_operation_plugin_api.h). Result callbacks receive validated metadata, requested tensor coverage, and host services; the registry rejects missing or mismatched Result tables before import, and callback exceptions are fenced at the host boundary.

`ExecutionContextConfig` bounds CPU worker count, queued callbacks, controlled live bytes, and optional caches. Ordinary callback submissions release their waiting slot when a worker starts the callback. A `CPU_STAGES` job keeps its admission until every submitted tile retires and the job is unlinked, so a running staged job can still prevent another submission when the waiting limit is full. See the [parallel execution model](../kernel-architecture/Parallel-Execution-Model.md) for the shared scheduler queue. `maximum_live_bytes` accounts controlled computation payloads, including regional reads and managed outputs, scratch, intermediates, and transfers. It does not represent process RSS or count caller-preexisting input storage. An allocation must reserve its budget before payload allocation, and its lease remains until the last storage owner releases it.

## 4. Non-Goals & Explicit Boundaries

- CPU regional execution is required; GPU backends are optional and operation-specific. GPU availability does not imply every operation can execute there.
- The controlled byte limit covers instrumented payload allocations. It does not bound caller allocations, uninstrumented metadata, thread stacks, or operating-system overhead.
- `Region` describes logical coverage. It does not promise that a general strided `Value` is physically contiguous.
- A planar image's channel grouping describes logical samples and semantic components; it does not convert planar storage into interleaved bytes.
- The executor does not roll back tiles already consumed by a streaming sink. Streaming has no persistent commit protocol.
- Trusted in-process operation code is not sandboxed. Allocation services and ABI checks enforce the callback contract but do not contain arbitrary native code.

## 5. Consequences

Invalid, empty, or out-of-bounds output requests fail during planning. A callback may be rejected when queue admission, workspace reservation, or output allocation cannot be satisfied. Work that can proceed within current leases waits for worker capacity. Cancellation is cooperative; after valid entry, observed cancellation takes precedence over stale graph state and ordinary operation failure. An operation failure does not publish its incomplete output, and execution failure prevents ordinary `execute` from returning a partial collected result.

Large whole-operation working sets can require more memory than a small requested output because the operation establishes a full-materialization boundary. Callers should select region-aware operations and tile geometry when the operation contract permits them, configure realistic resource limits, and release retained results when they no longer need them. Shared storage is charged once while shared; externally retained results keep their accounting lease alive.

`execute_stream` delivers named Value-based output tiles in deterministic name and spatial order, one synchronous sink call at a time. The `ValueView` expires when the sink returns; the sink can copy bytes or call `ValueView::retain()` to keep an owning `Value`. Retained outputs keep their storage leases alive and can exhaust the execution budget. Sink failure, cancellation, or staleness stops further delivery, and admitted callbacks retire before the call returns. Previously consumed tiles remain consumed even if a later tile fails. Memory limits do not bound process RSS, and operation/backend support depends on each operation's registered traits and configured backend.
