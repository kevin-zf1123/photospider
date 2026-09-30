# Compiler and Local Execution

## 1. Core summary (TL;DR)

The compiler validates a workflow and lowers it into a local plan that can be reused with independent bindings. Each execution call creates a private Run while sharing the context's bounded CPU workers, optional native lane, operation registry, caches, and resource ledger. The Run publishes named results only after callback outputs, cancellation, and graph currentness pass validation.

## 2. Mental model and intuition

```text
document + static resources
          |
     GraphSnapshot -> analyze -> SemanticGraphIR
                                   |
                             optimize (copy)
                                   |
                                  plan
                          output Region demands
                                   |
bindings ------------------------> Run
                           /                 \
                  ready CPU callbacks   ready GPU callbacks
                     compete for          serialized by one
                   bounded CPU pool       configured lane
                           \                 /
                     join + validate + final stop check
                            /                 \
                    success                   failure / stale / cancelled
                       |                                  |
       named Values / PlanarImages          discard outputs + typed status
```

The compiler derives dependency order and metadata before pixel work starts. Planning propagates each requested named output Region backward through Whole, Elementwise, and clipped Halo rules. The Run schedules ready steps and transfers only demanded data; generic regional sources and planar images can therefore supply data lazily.

## 3. Contracts and interfaces

```cpp
class Compiler {
 public:
  Result<SemanticGraphIR> analyze(const GraphSnapshot&,
                                  ResourceBindings = {}) const;
  Result<OptimizedGraphIR> optimize(const SemanticGraphIR&) const;
  Result<ExecutionPlan> plan(const OptimizedGraphIR&,
                             const PlanningOptions& = {}) const;
};

class ExecutionContext {
 public:
  Result<ExecutionResult> execute(
      const ExecutionPlan&, ExecutionBindings = {},
      const CancellationToken& = CancellationToken(),
      const ExecutionOptions& = {});
};
```

These are abbreviated public method declarations; unrelated members are omitted. `analyze` checks snapshot currentness, graph structure, operation availability, closed typed parameter schemas, input/output contracts, and inferred descriptors before publishing an immutable `SemanticGraphIR`. Missing, unknown, or wrong-type parameters fail analysis; operation defaults must be supplied by the caller or represented by the operation contract. `ResourceBindings` supplies immutable owners required to resolve static facets.

Operation definitions may specialize template traits against complete static input metadata through `specialize_metadata` when `requires_metadata_specialization` is set, or create an immutable prepared program through `prepare_static`. Static preparation is limited to deterministic, side-effect-free operations. The registry invokes these pure hooks outside its lock, without reading Value payloads. Resolved output metadata and workspace bounds participate in stage identity. A `PreparedOperation` retains its definition/library owner; separate calls do not share preparation unless the caller supplies the matching owner. Preparation and plan storage use host allocations outside per-observation runtime scratch admission. See [Plugin ABI](Plugin-ABI.md) for callback and owner lifetime rules.

`optimize` currently copies the semantic nodes into a separate immutable stage and computes a separate digest. `plan` copies operation traits into dependency-ordered local steps, applies caller placement options, estimates resource use, and derives each step's output and input demands. Digests and cache keys support identity checks; they are not security boundaries. Plans contain no callback pointer, native device handle, daemon object, or runtime input address. Execution requires the same frozen operation registry that produced the plan.

`ExecutionBindings` contains exactly named input entries. In ordinary generic execution, an input supplies one `Value`, `RegionalSource`, or `InputSnapshot`; a planar declaration supplies `PlanarImage`. In planar execution, a non-planar declaration accepts a matching `Value` only, while a planar declaration requires an explicit `PlanarImage`. Execution checks binding names and metadata before callbacks. It validates planar descriptor, facets, storage layout, and plan tile geometry at binding time. Data-dependent samples are validated when their requested regions are read.

`ExecutionContext` owns a fixed CPU pool and, when a configured native device is available, one GPU callback worker. Both lanes use deterministic FIFO queues and a shared nonblocking waiting-callback bound. A worker releases the waiting admission when it starts an ordinary callback, so running callbacks do not occupy that bound. A staged CPU job holds its waiting slot until its tile callbacks retire and it leaves the queue. Per-Run `maximum_parallelism` limits the number of in-flight plan steps. CPU callbacks can run concurrently; the native callback lane runs one callback at a time.

The planner schedules `Whole` operations over complete logical inputs and materializes their result at that boundary. A CPU Whole callback may use the explicitly granted parallel range service. Regional `Elementwise` and `Halo` work runs for requested tiles; each tile callback is one indivisible scheduled task and does not receive that range service. `ExecutionContext::execute_stream` delivers non-planar named Value output tiles synchronously to a borrowed sink in name/row/column order. Demand-driven operations use the separate `open_demand`, `execute_fragments`, and `execute_atoms` entry points; their continuation, query, and per-poll limits are described in [Dependency Data](Dependency-Data.md).

Each Run establishes a first-failure result under its mutex. At scheduler, queue, and callback boundaries it observes cancellation first, graph staleness second, and then the original failure. A worker checks this stop state before transferring dependencies, reserving buffers, or invoking an operation. If the Run has stopped, that worker retires its own in-flight slot and admits no new work. Callbacks already entered may continue; the executor waits for them to retire. Cancellation is cooperative and does not preempt arbitrary in-process code.

Operation callbacks receive planned input demands. Before a Value is transferred or passed to a callback, execution verifies that its Region covers the consumer demand. It validates each produced Value against the planned type, shape, and output demand before making it available to successors. The Run assembles the complete named output set into one `ExecutionResult` only on success. Failure, staleness, or cancellation discards locally assembled outputs and returns a typed status.

Direct registry invocation validates operation lookup, port and demand counts, each input Value before reading its descriptor, demand bounds, parameters, cancellation, backend vocabulary and capability, then static descriptor compatibility. Preserve/Match conflicts fail before user code. The registry derives the expected output descriptor before callback entry and checks the returned Value against it. The Run separately checks plan-derived coverage. Whole callbacks that require affine input views return `ViewUnavailable` before entry when an input demand spans incompatible owners; automatic collection is available only when the traits allow it. See [Plugin ABI](Plugin-ABI.md) for input-view, output-sink, and sticky failure rules.

For native execution, `ExecutionMode::NativeGpu` permits operations whose copied traits declare support for the configured backend. The plan includes typed upload, operation, and host-access actions. The Run checks packed upload size against the plan and reuses compatible retained buffers. Host access to CPU-accessible shared storage does not imply a copy. Native allocations charge actual backing capacity to the shared resource ledger; CPU reservations, callback workspace, transfers, cache entries, and planar backing follow their respective resource limits.

An optional native operation can fall back to CPU only when its traits permit fallback and the GPU callback reports `BackendUnavailable` before publishing output. If output publication has begun, backend unavailability is terminal. Submitted GPU work drains before callback retirement, including during cancellation. Diagnostics distinguish attempts, dispatches, transfers, cache reuse, and observed memory peaks.

The installed operation C ABI is version 11, exposed by `ps_operation_plugin_get_api_v11`; the structural planar C extension is version 3. Its `ps_gpu_service_v11` is synchronous and callback-thread-only. Service pointers and tokens expire when the callback returns; submitted dispatches drain before return, and service errors remain sticky. The ABI validates table layout and capabilities for trusted in-process modules. It does not isolate native code. MSL dispatches target Metal and SPIR-V dispatches target Vulkan; a backend/format mismatch returns `BackendUnavailable`. Each operation declares its numerical behavior.

Returned Values and PlanarImages keep their storage owners and resource leases after the Run or context ends. Temporary Values retire after their final reader. Releasing the final returned owner releases retained capacity. Caller-owned input payloads are retained by a Run but are not newly allocated execution buffers; controlled outputs, scratch, transfers, and intermediates are charged to the context ledger.

## 4. Non-goals and explicit boundaries

- The kernel does not own daemon sessions, IPC, durable Jobs, process isolation, or result recovery.
- Internal IR, plan objects, and stage digests are not serialization formats or security proofs.
- Native operation modules execute as trusted code in the host process.
- GPU mode does not guarantee native support for every operation or that all image output stays device-resident across nodes.
- Diagnostics report observed work and resource use; they do not certify numerical correctness or release readiness.

## 5. Consequences

Invalid graph structure, parameters, bindings, or operation capabilities fail before the affected callback starts. Queue and resource limits can reject work under contention; the caller must handle typed admission failures and release results it no longer needs. A slow or non-cooperative callback can delay Run completion after cancellation because the executor waits for callback ownership to retire. Holding results retains their storage leases and can reduce capacity for later Runs. ABI changes require rebuilding consumers against the matching installed package.
