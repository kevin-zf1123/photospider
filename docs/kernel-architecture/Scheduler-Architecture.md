# Scheduler Architecture

The kernel has a scheduler interface for dispatching concrete ready work. This
document defines the current worker, queue, lifecycle, plugin, intent-selection,
ready-task, and trace behavior.

## Current Interface: IScheduler

`IScheduler` is the formal scheduler interface and publicly derives from
`SchedulerTaskRuntime`. A scheduler attaches to a borrowed public
`SchedulerHostContext`, starts worker resources, schedules compute, and shuts
down cleanly. The context exposes only device capability, task worker/epoch
context, and trace publication; it never exposes `GraphRuntime`, graph state,
cache ownership, or a native device handle.

Core lifecycle:

```text
create -> attach(host_context) -> start -> submit ready callbacks -> shutdown -> detach
```

`GraphRuntime` implements and owns the host context plus this lifecycle ordering
for registered schedulers. A
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
The original `std::bad_alloc`, `std::system_error`, or local implementation
exception is then rethrown unchanged. Repeated `shutdown()` remains safe after failure, and the
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
`GraphRuntime::set_scheduler()` and `replace_scheduler()` use the same owner
transaction. They reserve any new map node before candidate lifecycle work, keep
the old owner published and alive while the candidate attaches and, for a
running runtime, starts, then publish the prepared candidate with a
non-allocating `unique_ptr` swap. Candidate attach/start failure triggers
independently fenced shutdown and detach; secondary cleanup errors are
suppressed and the exact preparation exception survives. After successful
publication, the displaced owner is shut down, detached, and destroyed in that
order. A displaced-owner cleanup error is reported after both stages without
rolling the new owner or the runtime running state back.
`GraphRuntime::stop()` publishes stopped state under the same lifecycle mutex,
treats each scheduler's `is_running()` query and `shutdown()` call as separate
best-effort lifecycle steps, and rethrows the first failure only after
completing the sweep. If the state query throws, the runtime records that error
but still attempts the same scheduler's shutdown because its state is unknown;
later schedulers are swept regardless of either failure. Its `noexcept`
destructor suppresses an explicit plugin lifecycle failure while scheduler
owners retain their own final cleanup fences.

The host-side plugin scheduler owner re-arms those final cleanup fences before
entering every `attach()` or `start()` attempt. A repeated attach or start may
publish a borrowed host pointer, workers, or another partial plugin resource
and then throw. In that case, owner destruction still performs an independently
fenced detach or shutdown before the single plugin destroy call; completion of
an earlier lifecycle never suppresses cleanup required by the failed retry.

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
The executor's virtual destructor is protected: scheduler code may run the
borrowed object but cannot delete dispatcher-owned storage through its base
pointer.

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

Active epoch and completion-count publication share one counter mutex with
increment/decrement. Initial batch publication stages queue state first and
then publishes the new nonzero epoch and count atomically, so an old callback
cannot pass an epoch check and mutate a new batch. Epoch advancement wraps
`UINT64_MAX` to `1`; zero remains reserved for uncancellable compatibility
work. A decrement at zero is a no-op, a nonpositive increment is a no-op, and a
positive increment beyond `INT_MAX` throws `std::overflow_error` without state
change. Any nonzero worker TLS epoch different from the active epoch is stale,
so its increment or decrement is ignored even across wrap.
Exception consumption uses the same counter mutex and clears a remaining count
only when the waiter's captured epoch is still active, so a late waiter cannot
zero a newer batch.

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
and the first-publisher latch. A null `set_exception` input returns before
claiming publication or mutating queues, counters, epochs, or exception state.
GPU pipeline CPU workers use one shared mutex and
one `rt_cv_` predicate handshake for both the RT queue and HP-CPU fallback
queue; every publisher changes either ready predicate under that same mutex
before notifying. There is no separate unwaited `hp_cpu_cv_`, so HP submission
cannot be lost between predicate evaluation and condition-variable sleep.
Shutdown remains a separate lifecycle transition; the GPU pipeline publishes
its stop state while holding the CPU idle-queue, GPU idle-queue, and
completion-wait mutexes before notifying and joining workers.

`GraphRuntime` stores a scheduler map keyed by `ComputeIntent` and current
compute selects either the `GlobalHighPrecision` or `RealTimeUpdate` entry.
The key selects a per-graph scheduler object; it does not itself define QoS,
deadline, fairness, cancellation, resource reservation, or a process-wide
priority class. Those concepts are not inferred from `ComputeIntent` in the
current contract.

Schedulers do not pull plans. `ComputeTaskDispatcher` discovers readiness and
pushes concrete callbacks or borrowed task handles through
`SchedulerTaskRuntime`. The selected scheduler may order and route those ready
submissions using its queues, priority hints, epoch, and available devices, but
it never receives graph topology, a compute plan, or dirty-propagation
ownership.

For `RealTimeUpdate`, `IntentUpdateCoordinator` launches the RT dirty sibling
and then the HP dirty sibling through separate asynchronous calls, allowing
them to compute concurrently when both selected scheduler runtimes are running.
RT writes are staged in
`RealtimeProxyWriteBuffer` and committed to `RealtimeProxyGraph`; HP writes are
staged in `HighPrecisionDirtyWriteBuffer` and commit to `GraphModel` only after
the RT proxy commit gate opens. Each scheduler handles the ready callbacks,
scheduler-local batch epochs, and queue policy for its sibling; it does not
create the siblings or make RT output a formal HP cache source.

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
registration, events, and platform resources. It does not expose a general
worker queue, task graph, or completion-counter API. Graph-state operations and
compute requests that mutate the visible `GraphModel` use
`GraphStateExecutor`, including scheduler-backed parallel compute. Scheduler
implementations interact with the runtime only through `SchedulerHostContext`;
neither `IScheduler` nor `SchedulerTaskRuntime` obtains direct ownership of the
runtime model.

The same executor is the scheduler-owner access and teardown boundary. Runtime
start, compute, scheduler name/statistics copying, and scheduler replacement
cannot overlap for one session. Graph close stops lane admission, drains its
bounded FIFO, and joins the sole lane worker before runtime stop invokes any
scheduler lifecycle method. `get_scheduler()` may return a raw pointer
internally, but its caller finishes all use while the graph-state callback is
active; replacement cannot publish and destroy the old owner until active
compute has released it.

## Built-in Schedulers

| Type | Meaning |
| --- | --- |
| `cpu_work_stealing` | Multi-threaded CPU scheduler. |
| `serial_debug` | Deterministic single-threaded debugging scheduler. |
| `gpu_pipeline` | CPU scheduler with optional GPU HP queue and device availability reporting. |
| `heterogeneous` | Alias for `gpu_pipeline`. |

`GpuPipelineScheduler::Config` currently exposes only active queue controls:
`cpu_workers`, `gpu_workers`, and `prefer_gpu_for_hp`. Scheduler submissions do
not carry their outer `ComputeIntent`. High-priority work enters the CPU queue
named for RT work; normal-priority work may enter the GPU queue when
`prefer_gpu_for_hp=true`, GPU workers are configured, and
`SchedulerHostContext` reports `GPU_METAL` available. Both HP and RT dirty
source batches currently use high priority, and both downstream groups use
normal priority, so queue naming must not be interpreted as an intent contract.
Older fields such as
`force_cpu_for_rt`, `rt_preempt_threshold_ms`, and scheduler-local
implementation priority tables are not active configuration in the current
code path.

### Per-graph physical resource ownership

Each `GraphRuntime` owns a scheduler object for HP and another for RT. Graph
load creates both objects and runtime start starts both. A configured worker
request is valid only from zero through eight. Before scheduler or plugin
construction, zero resolves once to:

```text
min(max(1, hardware_concurrency()), 8)
```

An explicit value from one through eight remains exact; a larger request is
rejected before worker construction and does not change future-Graph defaults.
The built-in CPU scheduler owns no more than the resolved grant. A registered
plugin is charged the full resolved grant and may own fewer workers but not
more. `gpu_pipeline` and `heterogeneous` are charged the resolved CPU grant plus
their one configured potential GPU worker, even when the device is unavailable
at admission time. Their absolute current instance charge is therefore nine.
`serial_debug` is the sole zero-slot exception and executes synchronously on
the calling thread.

These schedulers still own physical resources per graph and intent; there is no
shared worker pool or cross-graph fairness mechanism. The current containment
layer instead uses one process-lifetime `SchedulerWorkerBudget` with a fixed
32-slot ceiling shared by every embedded `Host` and `Kernel`. Graph load plans
both intent schedulers and reserves their combined charge atomically before
constructing either. The returned pair contains one move-only reservation per
intent; each remains outside the concrete scheduler during construction, then
moves into its `ReservationOwnedScheduler`. `GraphRuntime` remains responsible
for scheduler shutdown and detach; the reservation owner guarantees only that
concrete scheduler destruction precedes slot release. Failed attach/start/YAML
publication, successful graph close, and Host destruction therefore return
every acquired slot exactly once. A failed close instead retains the runtime
and both reservations for retry.

Scheduler replacement is a strong transaction: it plans and reserves the
candidate while the old scheduler and reservation remain live, then
attach/starts the candidate and publishes it before retiring the old owner.
Consequently replacement requires transient headroom. Capacity exhaustion is
reported as `GraphErrc::ComputeError` without constructing a candidate and
without disturbing old compute behavior; invalid requests and unknown types
remain `InvalidParameter`. This 32-slot ledger bounds only workers represented
by built-in planning or the trusted plugin grant. It does not bound graph-state
executors, daemon threads, frontend helpers, operation-internal threads, or all
operating-system threads in the process. `GraphStateExecutor` has a separate
structural bound: one worker and at most 64 waiting callbacks per loaded Graph.

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

Every scheduler DSO must first return numeric ABI version 2 from
`ps_scheduler_plugin_get_abi_version() noexcept`. Missing or mismatched
handshakes fail before implementation version, discovery metadata, or instance
creation is called; ABI v1 has no compatibility path. The implementation
version is then queried once and cached as diagnostic metadata, not used as a
compatibility gate. On creation, `num_workers` is the already-resolved
one-through-eight hard grant. A conforming in-process plugin may own fewer
worker threads but must never exceed that grant, while the Host conservatively
retains the full grant until concrete instance destruction. The counter is not
a sandbox and cannot stop a hostile or nonconforming DSO from creating
unreported threads.

Successful discovery additionally requires a positive type count, a non-null
and non-empty stable name at every in-range index, and at least one valid name
that does not conflict with an existing scheduler type. Invalid identity
metadata rejects the whole candidate with no type, metadata, or retained-handle
publication. A candidate with some conflicts still commits atomically when at
least one non-conflicting type remains.

Every discovery, create, lifecycle, and runtime call into a scheduler DSO holds
an explicit lease and converts plugin-origin exceptions to host-owned values:
fresh `std::bad_alloc`, copied `GraphError` code/message,
`std::invalid_argument` as `InvalidParameter`, and other standard/unknown
failures as `ComputeError`. Returned device enumerators are validated before
planning. Host task failures remain exact and plugin-visible: the owner
preallocates append-only identity slots, relays record the original
`exception_ptr` without allocation and use bare `throw;`, and the host restores
only a matching registered identity. Plugin code executes without the registry
guard, wait clears the batch registry, and exception objects are destroyed only
after the guard is released so their destructors may safely reenter scheduler
APIs.

## Daemon Host-Only Scheduler Boundary

`photospiderd` routes scheduler discovery and control only through copied
public `Host` values. The process-global methods are `scheduler.types`,
`scheduler.description`, `scheduler.scan`, `scheduler.load`,
`scheduler.loaded_plugins`, and `scheduler.configure_defaults`. None resolves
a graph session. The first five inspect or mutate the process-owned scheduler
factory/loader state; configuration changes only the defaults used for graph
sessions loaded later. Existing sessions keep their scheduler objects until an
explicit replacement.

`scheduler.info` and `scheduler.replace` first validate the opaque daemon
session id, `ComputeIntent`, and replacement type where applicable, then retain
one session admission through exactly one matching Host call. The embedded
Host executes scheduler name/statistics copying and replacement inside the
same per-graph `GraphStateExecutor` boundary as compute and graph close. A
running compute therefore cannot overlap scheduler inspection or replacement
for that graph, and replacement cannot destroy the displaced scheduler until
the active compute releases it.

Every direct request and every first-page access uses the daemon's common Host
mutex. Scheduler mutation is never retried. `scheduler.types` and
`scheduler.loaded_plugins` reserve bounded collection quota before their one
Host call, validate and sort the complete copied list while preserving
duplicates, and freeze it for stable cursor paging. A continuation reads only
that frozen process-global value and performs no Host call or session lookup.
This keeps socket IO outside Host locking while serializing scheduler access
with every other Host-routed family.

Successful scheduler plugin libraries remain owned by the process loader
across client disconnects and graph sessions. Wire values expose only type
names, descriptions, diagnostic plugin labels, configuration values, and
copied scheduler name/statistics. They never expose a scheduler pointer,
factory, registry, loader, callback, dynamic-library handle, or mutable
ownership token. The full wire schemas and bounds are maintained in
`../codebase-structure/IPC-Protocol-v1.md`.

`scheduler.trace` remains a separate bounded non-destructive observation
route. The installed typed IPC Client exposes all nine scheduler routes,
aggregates the two frozen collection views with exact cursor/offset validation,
and performs each direct mutation or trace/status observation RPC once. All
nine routes remain members of the exact sorted 55-method
`daemon.version.methods` inventory. The installed
`create_ipc_host(socket_path)` maps the corresponding nine Host virtuals through
fresh short-lived typed connections, preserves the same one-attempt mutation/
observation policy, and never falls back to embedded scheduler state.

## Scheduler Dispatch Boundary

`IScheduler` exposes no compute-planning helpers. Scheduler implementations
therefore cannot own graph or task planning.

The current dispatch model is:

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

Current scheduler instances own queue selection, batching, worker lifecycle,
epoch filtering, and resource-specific dispatch. Scheduler runtimes also report
available devices through `SchedulerTaskRuntime::available_devices()`.
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
Scheduler implementations make decisions from generic submission metadata such
as scheduler-local epoch and optional scheduler-specific priority hints. Dirty
generation remains task/snapshot provenance and is not passed as the scheduler
epoch by the current source-first dirty dispatch. A scheduler may drop stale
queued work by epoch, use FIFO/LIFO or work-stealing queues, route CPU/GPU
resources, or keep older work running. It does not receive task graphs or
require a bespoke dirty-source queue.

The compute-service path derives planned work before scheduler dispatch and
exposes no compute path outside planned-task dispatch. An
empty planned dispatch is acceptable only when the target already has reusable
HP output; otherwise it is a planning contract error.

## Epoch and Cancellation

Scheduler queues use epochs to cancel stale queued work. Epoch `0` is treated
as non-cancelable compatibility work. Only submissions carrying a real epoch
can be dropped as stale. Epoch filtering does not cancel a callback that is
already running and is not a general `ComputeRun` cancellation contract.

## Observability

`SchedulerHostContext::log_event` maps public `SchedulerTraceAction` labels to
the private `GraphRuntime::SchedulerEvent` ring in one host adapter. Built-in
and plugin schedulers publish through that context and do not duplicate the
mapping. The private ring records assignment, node execution, tile execution,
dirty-path, stale-generation, and exception-rethrow actions in a thread-safe
fixed-capacity ring. Each graph preallocates 65,536 production
slots. Valid publication sequences are `1..UINT64_MAX-1`; `UINT64_MAX` is the
terminal exhaustion sentinel and is never assigned. Full-ring eviction removes
exactly the oldest trace. Once the last valid sequence is consumed, later
publication attempts are dropped and counted with saturating arithmetic.

`Host::scheduler_trace(session, after_sequence, limit)` is a non-destructive
sequence-page read. Zero starts at the oldest retained event, and a valid limit
is 1 through 4,096. The returned `SchedulerTracePage` contains only events with
sequences greater than the cursor plus `next_sequence`, `has_more`, and an exact
saturating `dropped_count` for unavailable retained history and exhausted
publication attempts after that cursor. Page contents and metadata observe one
ring-lock point. Repeating a cursor does not remove or reorder trace events;
later publications may appear on a later read before exhaustion.

The page advances to the last returned sequence, retains the input cursor for
an empty pre-exhaustion page, and returns `UINT64_MAX` after the final valid
sequence is observed or exhausted storage has no later retained event. A
sentinel cursor is valid only after exhaustion. Invalid limits and future or
premature-sentinel cursors fail with `GraphErrc::InvalidParameter` before a page
is copied. Internal tests may clear retained trace slots, but production code
has no unbounded trace getter or public clear control.

Scheduler-worker capacity validation is separate from display statistics and
trace metadata. Long-lived integration tests observe synchronized real
operation callbacks reached through the public Host compute path. They do not
infer the process worker total from `get_stats()`, per-instance worker ids, or a
whole-process operating-system thread snapshot.

## Current Boundary Invariants

- `IScheduler` is the current formal public scheduler interface.
- Concrete ready parallel work is routed through scheduler-owned task runtimes;
  plans are not pulled by schedulers.
- Graph-state commands and visible graph compute requests remain behind
  a one-worker, 64-waiting-task `GraphStateExecutor` FIFO lane.
- Scheduler runtimes are ready-task-only: they receive concrete callbacks with
  scheduler-local epoch and optional scheduler-specific hints, not task
  graphs or dirty work-set state.
- Plugin scheduler lifecycle follows `Plugin-ABI.md`.
- Scheduler attachment is limited to `SchedulerHostContext`; the context does
  not expose graph/runtime ownership or native backend handles.
- Per-instance worker grants and the shared 32-slot admission ledger contain
  current per-graph ownership; they do not create a shared executor or claim a
  whole-process OS-thread limit.
- The current interface combines policy and physical worker ownership. ADR 0003
  records a different accepted ownership decision for later implementation;
  this document describes the currently executable contract.
