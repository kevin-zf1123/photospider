# Scheduler Architecture

The kernel has a formal scheduler interface for intent-aware resource dispatch.
This document defines how to read the current implementation after planned
parallel work was routed through scheduler-owned task runtimes.

## Formal Target: IScheduler

`IScheduler` is the formal scheduler interface. A scheduler attaches to a
`GraphRuntime`, starts worker resources, schedules compute, and shuts down
cleanly.

Core lifecycle:

```text
create -> attach(runtime) -> start -> dispatch planned tasks -> shutdown -> detach
```

`GraphRuntime` owns this lifecycle ordering for registered schedulers. A
scheduler is attached before it starts; `GraphRuntime::start()` starts
previously registered schedulers, `GraphRuntime::set_scheduler()` starts a new
scheduler only after attach when the runtime is already running, and
`GraphRuntime::stop()` shuts registered schedulers down. `Kernel` bootstrap code
must register schedulers through `GraphRuntime` instead of pre-starting them.
Scheduler shutdown must publish the stop state under the same synchronization
used by idle worker waits and completion waits before notifying those waiters.
This prevents shutdown from missing a worker that is transitioning into
condition-variable sleep after the final task in a batch has completed.

### Transactional start and runtime publication

`CpuWorkStealingScheduler::start()` and `GpuPipelineScheduler::start()` provide
the strong exception guarantee. Queue arrays, mutex ownership, and worker
vectors are staged before lifecycle publication. If resource allocation or any
CPU/GPU thread construction throws, the scheduler publishes `running=false`,
notifies every worker/completion wait, joins every thread already created by
that attempt, and clears staged arrays, queues, counters, and exception state.
The original `std::bad_alloc`, `std::system_error`, or plugin exception is then
rethrown unchanged. Repeated `shutdown()` remains safe after failure, and the
same scheduler object can retry `start()` and execute a later batch.
Even after staged threads exist, `is_running()` remains false until the complete
worker vector, queue arrays, mutex ownership, counters, and exception state have
been installed in member storage. Observers therefore cannot treat a partially
installed worker set as a running scheduler.

`GraphRuntime::start()` is the outer lifecycle transaction. It reserves a
rollback ledger and records each stopped scheduler before invoking that
scheduler's `start()`. If any start call fails after partial local publication,
the failing scheduler and all earlier schedulers started by the call are shut
down in reverse order. Secondary rollback errors are suppressed so the first
start error survives, and `GraphRuntime::running()` becomes true only after all
schedulers start successfully.
`GraphRuntime::stop()` publishes stopped state under the same lifecycle mutex,
attempts every scheduler shutdown even when an earlier plugin throws, and
rethrows the first failure only after completing that sweep. Its `noexcept`
destructor suppresses an explicit plugin lifecycle failure while scheduler
owners retain their own final cleanup fences.

Deterministic allocation and thread-creation hooks exist only in
`BUILD_TESTING=ON` objects. A `BUILD_TESTING=OFF` product contains no hook
storage, exported test seam, or forced GPU route.

### Transactional batch enqueue and borrowed handles

Every multi-task enqueue is a queue transaction. CPU high-priority/global-ready
and local work-stealing routes, plus GPU RT, HP-CPU, and GPU routes, retain the
relevant queue locks while recording original deque lengths and appending the
whole batch. If any insertion throws, appended entries are removed from the
back without allocation. Epoch, ready/completion/stat counters, exception
claim/pointer/epoch/cleanup/visible state, and condition-variable notifications
are published only after the complete batch commits. If an insertion fails,
all of those exception and epoch fields retain their exact pre-call values. No
worker can observe a prefix and the original exception identity propagates.

`TaskHandle` is a borrowed pair of executor pointer and task id. Its
`TaskExecutor` must outlive every successfully committed callback through
`wait_for_completion()`. A failed batch commits no handle and executes zero
callbacks, so request-local dirty executors may unwind immediately without
leaving a queue entry that points into destroyed stack storage. The scheduler
can accept the next batch on the same object after rollback. Exception
publication uses the same queue transaction gate before choosing its epoch,
so a concurrent batch is observed either wholly committed or wholly absent.

### Batch exception publication and reuse

The CPU work-stealing and GPU-pipeline runtimes publish one exact worker
exception per batch. A separate first-publisher latch is reset only when a new
batch starts. The winning publisher first verifies its executing task epoch
matches the active batch, stores the exact `first_exception_` and exception
epoch, rejects further ready submissions for that batch, and drains every
queued callback. Only after queue cleanup completes does it release-store the
consumer-visible `has_exception_` flag. The publisher retains the same initial-
submission queue gate through claim, pointer, cleanup, epoch, and visible-flag
publication. A new initial batch therefore cannot reset exception state between
the old pointer store and old flag store, eliminating cross-epoch publication.

Dequeued callbacks carry their scheduler batch epoch and increment an in-flight
count before becoming invisible to queue cleanup. Completion, completion-count
growth, and exception publication from a stale epoch are ignored. A completion
waiter does not return success until both the completion count and in-flight
count reach zero. It does not rethrow failure until queue cleanup is complete
and every old callback has settled. The waiter then reads and clears the exact
pointer/flag under the exception mutex. This settle-before-return policy makes
immediate next-batch submission safe: no old publisher remains able to drain a
new queue, decrement its count, or publish a late exception into it.

For CPU work stealing, a local-queue enqueue and its ready-predicate increment
hold the global predicate mutex before the target local mutex. A local dequeue
decrements the predicate before releasing that local mutex. Exception cleanup
uses the same global-to-local order, retains the global mutex across every
queue drain, and resets the ready predicate only after all local queues have
been visited. This makes queue visibility and the numeric wait predicate one
publication unit: neither a late increment after cleanup nor a decrement below
the reset value can leak into the next batch.

This ordering is part of scheduler reuse: an observed exception flag always
has a non-null matching pointer, the exact exception identity reaches the
waiting caller, and the next batch explicitly resets both publication state
and the first-publisher latch. GPU pipeline CPU workers use one shared mutex and
one `rt_cv_` predicate handshake for both the RT queue and HP-CPU fallback
queue; every publisher changes either ready predicate under that same mutex
before notifying. There is no separate unwaited `hp_cpu_cv_`, so HP submission
cannot be lost between predicate evaluation and condition-variable sleep.
Shutdown remains a separate lifecycle transition; the GPU pipeline publishes
its stop state while holding the CPU idle-queue, GPU idle-queue, and
completion-wait mutexes before notifying and joining workers.

Compute routes by `ComputeIntent`:

| Intent | Expected scheduler role |
| --- | --- |
| `GlobalHighPrecision` | Throughput-oriented HP compute. |
| `RealTimeUpdate` | Low-latency interactive update. |

`GraphRuntime` stores a scheduler map keyed by `ComputeIntent`.

The long-term scheduler responsibility is resource dispatch after planning.
Schedulers should pull planned or annotated tasks from intent-aware task pools,
choose queue order, batching, worker policy, cancellation, and concrete
execution resources, then dispatch work. They should not own graph-level
dirty-region propagation or compute-task derivation.

For `RealTimeUpdate`, the scheduler-backed path now starts the RT dirty sibling
before the HP dirty sibling and allows both siblings to compute concurrently
when both scheduler runtimes are running. RT writes are staged in
`RealtimeProxyWriteBuffer` and committed to `RealtimeProxyGraph`; HP writes are
staged in `HighPrecisionDirtyWriteBuffer` and commit to `GraphModel` only after
the RT proxy commit gate opens. Schedulers still handle ready task callbacks,
epochs, cancellation, and queue policy; they do not make RT output a formal HP
cache source.

## Current Dispatch State

Parallel compute planning and plan execution belong to `ComputeService`
collaborators: `FullTaskGraphExpander`, `NodeCacheTaskGraphPruner`,
`DirtyRegionPlanner`, `DirtySnapshotTaskGraphPruner`,
`IntentUpdateCoordinator`, and `ComputeTaskDispatcher`. After pruning,
`ComputeTaskDispatcher` materializes either the node/cache-pruned task graph or
the dirty-clipped update work set into concrete tasks and submits ready work
through the configured `IScheduler` instance for the relevant `ComputeIntent`
via `SchedulerTaskRuntime`.

`GraphRuntime` owns graph state, the `GraphStateExecutor`, scheduler
registration, events, and platform resources. It no longer exposes a general
worker queue, task graph, or completion-counter API. Graph-state operations and
compute requests that mutate the visible `GraphModel` use
`GraphStateExecutor`, including scheduler-backed parallel compute. The
scheduler-facing design should extend `IScheduler` and
`SchedulerTaskRuntime`; it should not bypass graph-state access by taking
direct ownership of the runtime model.

## Built-in Schedulers

| Type | Meaning |
| --- | --- |
| `cpu_work_stealing` | Multi-threaded CPU scheduler. |
| `serial_debug` | Deterministic single-threaded debugging scheduler. |
| `gpu_pipeline` | CPU scheduler with optional GPU HP queue and device availability reporting. |
| `heterogeneous` | Alias for `gpu_pipeline`. |

`GpuPipelineScheduler::Config` currently exposes only active queue controls:
`cpu_workers`, `gpu_workers`, and `prefer_gpu_for_hp`. The scheduler always
routes RT ready work to the high-priority CPU queue. Normal-priority HP ready
work may enter the GPU queue when `prefer_gpu_for_hp=true`, GPU workers are
configured, and a Metal device is attached to the runtime. Older fields such as
`force_cpu_for_rt`, `rt_preempt_threshold_ms`, and scheduler-local
implementation priority tables are not active configuration in the current
code path.

## Plugin Discovery vs Graph Selection

`graph_cli` scans `scheduler_dirs` during startup after config load and before
graph load. That scan only makes plugin-provided scheduler types available to
the scheduler factory. It does not change the scheduler instances already
selected for a graph.

Newly loaded graphs receive scheduler instances from
`Kernel::SchedulerConfig`, which is populated from `scheduler_hp_type`,
`scheduler_rt_type`, and `scheduler_worker_count`. A plugin scheduler therefore
becomes active only when one of those config keys names the scanned plugin type,
or when the user later runs `scheduler set <hp|rt> <type>` for the current
graph. `scheduler plugins` reports discovery state; `scheduler get all` reports
the actual HP/RT scheduler instances attached to the current graph.

## Scheduler Dispatch Boundary

`IScheduler` no longer exposes compute-planning helpers. Removed planning
interfaces must not be reintroduced, so scheduler implementations cannot
accidentally own graph/task planning.

The target model is:

```text
GraphModel topology
  -> FullTaskGraphExpander
  -> FullTaskGraph
  -> NodeCacheTaskGraphPruner
  -> ComputePlan / pruned ComputeTaskGraph
  -> DirtyRegionSnapshot
  -> DirtySnapshotTaskGraphPruner
  -> DirtyUpdateWorkSet
  -> Scheduler resource dispatch
```

Queue selection, batching, worker reservation, cancellation, and
resource-specific dispatch belong in the scheduler. Scheduler runtimes also
report available devices through `SchedulerTaskRuntime::available_devices()`.
`ComputeTaskDispatcher` uses that list with
`OpRegistry::select_best_implementation()` to choose operation callbacks that
match the already materialized task shape. Scheduler queue routing therefore
does not own operation implementation selection. Dirty propagation, dirty
work-set activation, node/tile expansion, monolithic dirty escalation, and
logical compute-task derivation belong before scheduler dispatch.

The dirty control lane is not a dirty-feature-specific scheduler queue. Dirty
nodes update graph-scoped dirty lifecycle and ROI state through a serialized
control path; the dispatcher materializes dirty work generations from that
state and submits only concrete ready task callbacks to the scheduler.
Scheduler implementations should make decisions from generic ready-task
metadata such as epoch, dirty generation, and optional scheduler-specific
priority hints. A scheduler may drop stale queued work by epoch, use FIFO/LIFO
or work-stealing queues, route CPU/GPU resources, or keep older work running,
but it should not receive task graphs or require a bespoke dirty-source queue.

The compute-service path derives planned work before scheduler dispatch and
does not expose a compatibility compute path outside planned-task dispatch. An
empty planned dispatch is acceptable only when the target already has reusable
HP output; otherwise it is a planning contract error.

## Epoch and Cancellation

Scheduler queues use epochs to cancel stale queued work. Epoch `0` is treated
as non-cancelable compatibility work. New interactive scheduling should assign
real epochs so obsolete RT work can be dropped.

## Observability

`GraphRuntime::SchedulerEvent` records assignment, node execution, and tile
execution events. This is useful for validating scheduler behavior.

## Development Direction

- Keep `IScheduler` as the formal public scheduler interface.
- Keep planned parallel work routed through scheduler-owned task runtimes.
- Keep graph-state commands and visible graph compute requests behind
  `GraphStateExecutor`.
- Keep scheduler runtimes ready-task-only: they receive concrete callbacks with
  epoch/generation metadata and optional scheduler-specific hints, not task
  graphs or dirty work-set state.
- Keep plugin scheduler lifecycle compatible with `Plugin-ABI.md`.
- Continue later work on richer annotated task pools, planner plugin ABI, and
  scheduler policy metadata without moving graph-level dirty planning into a
  scheduler.
