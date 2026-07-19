# Compute Boundaries

This document describes current software behavior and implementation ownership
inside the compute subsystem.

## Scope

The compute subsystem accepts one validated internal request, derives work for
one HP domain or coordinated HP/RT siblings, executes operations, and publishes
the intent-specific result. It does not own graph document persistence,
frontend rendering, daemon
transport, or process-wide operation plugin lifetime.

The public caller reaches compute only through `ps::Host`. The embedded adapter
translates public `HostComputeRequest` values into internal Kernel and
`ComputeService` requests. No public API exposes a `ComputeService`, plan, task
graph, or scheduler pointer. Request, propagation, planning, and execution
geometry remains `PixelRect`/`PixelSize` through `NodeExecutor`; OpenCV geometry
exists only inside a provider or algorithm implementation at the library call
that consumes it.

## Ownership Map

```mermaid
flowchart TD
  HOST["ps::Host"] --> ADAPTER["embedded Host adapter"]
  ADAPTER --> KERNEL["Kernel"]
  KERNEL --> BUDGET["process SchedulerWorkerBudget"]
  KERNEL --> GSE["GraphStateExecutor"]
  GSE --> SERVICE["ComputeService"]
  SERVICE --> RUN["request-owned HP ComputeRun"]
  SERVICE --> PLAN["planning and pruning collaborators"]
  PLAN --> DISPATCH["ComputeTaskDispatcher"]
  DISPATCH --> RUN
  DISPATCH --> RUNTIME["SchedulerTaskRuntime"]
  RUNTIME --> CALLBACK["ready TaskHandle or callback"]
  CALLBACK --> TEMP["Run-owned temporary or dirty HP staging"]
  RUN --> TEMP
  TEMP --> COMMIT["validated result commit"]
  COMMIT --> GRAPH["GraphModel or RealtimeProxyGraph"]
```

`GraphStateExecutor` owns current per-graph exclusion. Planning and dispatch
remain compute responsibilities even when ready callbacks execute on scheduler
workers.

The current exclusion mechanism is a bounded serial FIFO lane. Every accepting
`GraphStateExecutor` owns exactly one worker. Its queue holds at most 64 waiting
callbacks, excluding the at-most-one active callback, so a Graph owns at most
65 admitted graph-state callbacks. `submit()` blocks the caller while the queue
is full; it neither creates another lane worker nor drops or bypasses admitted
work. Producer fairness before admission is not guaranteed, but admitted work
executes FIFO.

Each submission returns a packaged-task future with the callable's exact value,
reference, `void` completion, or exception. Destroying that future neither
waits nor cancels the task; executor lifetime retains admitted work. A callback
cannot submit to or close its own lane: worker re-entry throws
`std::logic_error` before queue waiting. The sole worker owns the whole callback,
including scheduler submission, completion waits, and visible commit.

`close_and_drain()` is concurrent-call and repeat-call idempotent. It stops
admission, wakes full-queue producers with `std::runtime_error`, drains prior
work FIFO, and joins the worker before returning. Each caller waits for the
durable close generation that it joined; a failed-stop restart may reopen a
later accepting generation before a delayed caller wakes without trapping that
caller or creating a second worker. `GraphRuntime` performs the join before
scheduler teardown. If explicit close later fails in scheduler shutdown,
Kernel starts one replacement lane worker before returning the failure so the
retained session remains retryable. Different graphs have independent workers
and queues. The scheduler-worker ledger does not count these lane workers; its
32-slot ceiling covers only workers charged by scheduler planning.

## Current Collaborators

| Module | Current responsibility | Does not own |
| --- | --- | --- |
| `ComputeService` | Request validation, intent coordination, creation/settlement of one non-realtime HP Run, collaborator construction, and final result selection | Frontend values, worker threads, graph documents |
| `ComputeRun` | Immutable non-realtime HP descriptor, monotonic phase, exact terminal outcome, shared-control ownership of full-plan/temporary or standalone dirty-HP staging storage, stable leases, and full-HP composite task identity | Paired realtime grouping, Graph state, workers, authoritative revision commit, cancellation, or resource admission |
| `ComputeCachePolicy` | HP cache eligibility and cache-path decisions | Disk I/O ownership or operation execution |
| `NodeInputResolver` | Runtime parameters and ready image inputs | Graph traversal or output commit |
| `FullTaskGraphExpander` | Complete node/tile task shape for one graph generation and domain | Request target, cache pruning, dirty pruning |
| `NodeCacheTaskGraphPruner` | Target/dependency cone and cache-aware request plan | New node or tile task shapes |
| `ComputeDispatchPlanBuilder` | Cache-pruned high-precision plan and inspection record | Scheduler queues |
| `DirtyRegionPlanner` | Graph-scoped dirty propagation snapshot | Compute dependency counters |
| `DirtySnapshotTaskGraphPruner` | Active dirty work selected from an existing plan | Task expansion |
| `IntentUpdateCoordinator` | HP-only or HP/RT sibling semantics | Physical priority or worker ownership |
| `ComputeTaskDispatcher` | Dependency counters, ready release, temporary-result indexing, completion, exceptions, full HP commit, and dirty source-first submission helper | Run storage, graph topology derivation, dirty staged commit, or scheduler policy |
| `TaskSubmissionPlan` | Run-owned dense indexes, dependency state, exact-once task state, variants, result slots, and callback owner for one full HP request | Scheduler workers, Run terminal state, or dirty-path execution |
| `NodeExecutor` | Consistent monolithic and tiled operation invocation | Graph mutation policy |
| `ComputeMetricsRecorder` | Compute events, timing, benchmark events, and debug metadata | Scheduler trace ownership |
| `SchedulerFactory` | Resolve `0..8` worker requests and plan each scheduler's conservative slot charge before construction | Process capacity ownership or graph-state access |
| `SchedulerWorkerBudget` | Serialize one fixed 32-slot process admission ledger shared by all embedded Hosts/Kernels | Worker creation, scheduling policy, fairness, or whole-process thread counting |
| `ReservationOwnedScheduler` | Keep a move-only reservation live through concrete scheduler shutdown and destruction | Capacity planning or task-graph correctness |

Compute collaborators live under `src/lib/compute/`; the three admission and
ownership collaborators live under `src/lib/scheduler/`. These classes are
private implementation modules and do not form an installable API.

## Request Behavior

1. `Kernel` resolves the session and enters the graph-state access boundary.
2. `ComputeService` validates target, intent, dirty ROI, cache flags, and the
   selected execution strategy.
3. For non-realtime HP, `ComputeService` creates one `ComputeRun` before
   planning, capturing a fresh id, session identity, topology-only submission
   revision, target, HP intent, full quality, and explicit QoS.
4. Connected parameter producers are stabilized into one request-local HP
   snapshot before extent, ROI, or task-shape decisions use them.
5. The planner expands the complete task shape for one domain and prunes it to
   the requested target and dependency cone.
6. A dirty request selects an active work set from that plan. Dirty state does
   not create new task shapes.
7. Sequential execution walks the same request semantics inline. Parallel full
   HP execution materializes owned callbacks that retain a Run lease and a
   `(RunId, RunLocalTaskId)` identity, then submits only ready callbacks to the
   selected scheduler runtime. Dirty execution retains its separate
   request-local handle path.
8. Workers write Run-owned full-plan temporary results or standalone dirty HP
   staging. Paired realtime staging remains sibling-callback-local. Visible
   graph state is modified only by the appropriate commit path.
9. The Run publishes one success or failure after validated output or exact
   exception capture, then the result, events, timing, and errors are copied
   back through the Host value boundary.

## Planning Invariants

- Full expansion is keyed by graph topology generation, compute intent, and
  task-shape configuration.
- A force-recache request invalidates reusable expansion when current input or
  parameter state may change output extent without changing topology.
- Request target, cache availability, and dirty state prune existing task
  shapes; they do not redefine graph topology.
- A `ComputeTaskGraph` is immutable while a scheduler-visible callback derived
  from it may still execute.
- HP and RT are separate compute domains. One plan does not create cross-domain
  task dependencies.
- Host, graph, planning, dirty work-set, staged-write, and `NodeExecutor`
  boundaries carry kernel-owned `PixelRect`/`PixelSize`, never OpenCV geometry.
- Tiled input normalization occurs once per node invocation where possible,
  rather than once per tile callback.

These rules make planning deterministic and keep the scheduler independent of
graph semantics. Planning cost therefore follows full expansion before
pruning. Lazy task creation is not part of the current planning contract.

## Dispatcher and Scheduler Boundary

The dispatcher owns request correctness while `ComputeRun` owns the current
full HP storage:

- dependency counters and dependent maps;
- source-first dirty task release;
- task reference accounting;
- indexing and transitions over Run-owned temporary result slots;
- exception normalization and completion aggregation;
- validation of an empty plan;
- final target selection and full HP commit; dirty executors own their staged
  commit after reusing the source-first submission helper.

The scheduler owns the current physical execution mechanism:

- worker lifecycle and ready queues;
- batch state and scheduler-local epoch filtering;
- implementation-specific task ordering;
- scheduler completion and exception publication;
- bounded trace publication through the Host context.

The scheduler never receives `GraphModel`, `ComputeTaskGraph`,
`DirtyRegionSnapshot`, or cache authority. Newly ready dependent work is
released by the dispatcher and pushed as another ready handle or callback.
Threaded scheduler resources are owned per `GraphRuntime` and per intent route;
there is no process-wide worker pool or cross-graph fairness authority. There
is a process-wide admission authority: graph load atomically reserves the
combined HP+RT charge, and replacement reserves one candidate charge while the
old owner remains live. The built-in serial scheduler charges zero; built-in
CPU and registered ABI v2 plugins charge the resolved one-through-eight grant;
built-in GPU/heterogeneous also charges its potential device worker.

## OpenCV Operation Concurrency

Repository-owned CPU OpenCV operations are reentrant provider work. The
built-in provider has no process-wide operation mutex. Its monolithic
`convolve`, `resize`, `crop`, `extract_channel`, `gaussian_blur`,
`add_weighted`, `abs_diff`, and `multiply` callbacks, together with tiled
`curve_transform`, `gaussian_blur`, `add_weighted`, `abs_diff`, and `multiply`,
may run concurrently across tiles, Graphs, and HP/RT intent routes. Callback
inputs are immutable; mutable `cv::Mat` headers, temporaries, and output regions
are callback-local or task-owned.

The same rule applies at the registry boundary. Registry locks serialize
ownership mutation, publication, coherent snapshot capture, and unload, but
they are released before callback invocation. Every provider must therefore
make its callback reentrant or synchronize its own shared mutable state. A
shared operation key, device, intent, or callback owner never implies
single-threaded execution.

The optional OpenCV provider calls `cv::setNumThreads(1)` exactly once before
publishing its callbacks. It uses `cv::Mat`, does not call
`cv::ocl::setUseOpenCL(false)`, and does not reconfigure OpenCV threading while
callbacks may be active. Its callback fence catches every `cv::Exception`
raised by a registered algorithm while still inside provider code. OpenCV
resource exhaustion becomes a fresh `std::bad_alloc`; every other OpenCV
failure becomes a host-owned `GraphError` with `GraphErrc::ComputeError`. The
admitted scheduler worker grant is therefore the repository-owned outer CPU
parallelism layer, while OpenCV internal CPU parallelism remains disabled.

`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF` omits this provider's
callbacks but leaves dependency-neutral core operations registered. The
registry and v2 registrar do not depend on OpenCV: another provider can publish
the absent operation, or replace an enabled OpenCV operation through the same
slots. Manager-driven unload retires the replacement and restores the captured
predecessor.

Synchronization around genuine backend state remains provider-local. The
Metal Perlin provider retains a DSO-private mutex around its shared Metal
device, queue, pipeline, and buffers; that mutex is neither an OpenCV operation
lock nor a scheduler exclusivity contract. OpenCV use outside repository-owned
providers, third-party internal threads, and platform runtime workers remain
outside scheduler worker accounting.

[ADR 0004](../adr/0004-opencv-cpu-operations-are-reentrant-provider-work.md)
records this decision. Durable integration coverage proves exact callback
overlap for `1/2/4/8` grants and bitwise-equal one-versus-eight-worker output;
the manual native scaling evidence is documented in
`../development/Testing-and-Validation.md`.
[ADR 0002](../adr/0002-external-libraries-are-kernel-adapters.md) and the exact
[dependency-neutral kernel target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel)
place OpenCV algorithms, codecs, exception translation, and process state
inside an optional provider/adapter instead of letting them define target
kernel semantics.

## Intent and Commit Boundaries

`GlobalHighPrecision` and `RealTimeUpdate` describe business semantics, not
resource policy. A real-time update coordinates an RT proxy sibling and an HP
authoritative sibling. Each sibling has its own domain plan, dirty snapshot,
staged output, and scheduler selection.

`IntentUpdateCoordinator` creates the current sibling concurrency with two
asynchronous calls. The selected schedulers execute ready work inside each
sibling; they do not create the sibling relationship or infer it from task
metadata.

The current normal compute policy holds per-graph exclusive access through
visible commit. Dirty paths already use narrower staged buffers:

- `RealtimeProxyWriteBuffer` commits only to `RealtimeProxyGraph`;
- `HighPrecisionDirtyWriteBuffer` commits authoritative HP output to
  `GraphModel` after the sibling commit gate opens.

This staging prevents partially assembled tile output from becoming visible.
It is not yet a general cancellation or graph-revision policy.

## Failure and Lifetime Semantics

- Invalid targets, intent/ROI combinations, planning contracts, and operation
  failures are reported through categorized graph errors and Host status
  values.
- Resource exhaustion may propagate as `std::bad_alloc` across documented
  non-destructor Host boundaries.
- An above-eight worker request or unknown scheduler type fails before worker
  construction as `InvalidParameter`; process-ledger exhaustion at graph load
  or replacement preserves `GraphErrc::ComputeError`.
- Scheduler reservations outlive their concrete workers during teardown:
  candidate rollback returns only candidate capacity, successful graph close
  or Host destruction returns retained capacity exactly once, a failed close
  retains it for retry, and replacement requires transient headroom while
  preserving the old scheduler on failure.
- An admitted scheduler batch is settled before its exception escapes the
  current request.
- Operation callbacks may already have external side effects; staged graph
  output does not roll those effects back.
- Scheduler-backed full HP work no longer borrows a raw `TaskExecutor`.
  `TaskSubmissionPlan` owns its runner, every real ready task is an owned
  callback retaining a `ComputeRunLease`, and failure publication must match
  `(RunId, RunLocalTaskId)`. The empty borrowed-handle batch only establishes
  the current scheduler epoch. Request-local dirty executors still use their
  separate synchronous borrowed-handle path and therefore cannot move
  unchanged into a process-wide asynchronous queue.

## Boundary Rationale

Separating planning, ready detection, physical execution, and commit provides
four independent correctness points:

1. Graph and ROI semantics can be tested without a worker pool.
2. Scheduler implementations can change ordering without owning Graph state.
3. Temporary output can be validated before becoming visible.
4. Physical execution ownership remains separable from dependency correctness.

[ADR 0003](../adr/0003-process-owned-execution-resources.md),
[ADR 0007](../adr/0007-compute-runs-and-process-execution-have-separate-owners.md),
and the exact
[process execution domain target](../roadmap/Kernel-Evolution.md#process-execution-domain)
record the accepted replacement direction and detailed ownership contract.
This document is authoritative for the current issue #67 HP Run lease and
completion-isolation slice, per-graph scheduler ownership, and bounded process
admission containment. Authoritative revision, paired realtime Runs, general
dirty-path leases, and the shared `ExecutionService` remain future behavior.

## Implementation and Validation Entry Points

- `src/lib/compute/compute_service.*`
- `src/lib/compute/compute_run.*`
- `src/lib/compute/task_graph_planning.*`
- `src/lib/compute/compute_dispatch_plan_builder.*`
- `src/lib/compute/compute_task_submission.*`
- `src/lib/compute/compute_task_dispatcher.*`
- `src/lib/compute/dirty_region_planner.*`
- `src/lib/compute/dirty_update_executor.*`
- `src/lib/compute/intent_update_coordinator.*`
- `src/lib/core/ops.cpp`
- `src/lib/providers/configured_operation_providers.*`
- `src/lib/providers/opencv/*`
- `src/lib/scheduler/scheduler_factory.*`
- `src/lib/scheduler/scheduler_worker_budget.*`
- `src/lib/scheduler/scheduler_reservation_owner.*`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_scheduler.cpp`
- `tests/integration/test_scheduler_worker_budget.cpp`
- `tests/unit/test_scheduler_factory_plan.cpp`
- `tests/unit/test_scheduler_reservation_owner.cpp`
- `tests/unit/test_scheduler_worker_budget.cpp`
- `tests/unit/test_compute_run.cpp`
- `tests/unit/test_propagation_contracts.cpp`
