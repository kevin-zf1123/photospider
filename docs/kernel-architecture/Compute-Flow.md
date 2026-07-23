# Kernel Compute Flow

This document describes current compute request, planning, execution, HP/RT,
commit, event, and error behavior. Module ownership is summarized in
`Compute-Boundaries.md`.

## Entry Points

Typical frontend flow:

```text
CLI / TUI
  -> ps::Host
  -> embedded Host adapter
  -> InteractionService
  -> Kernel
  -> GraphRuntime
  -> ComputeService
  -> OpRegistry / GraphCacheService / GraphTraversalService
  -> RoiPropagationService / GraphExtentResolver
```

`Kernel` owns the multi-graph API. `GraphRuntime` owns one graph model, a
per-graph visible-state `GraphStateExecutor`, a separate bounded serial
compute-request lane, one per-live-Graph `ComputeRequestCoordinator`, event
service, platform context, and one copied execution-route binding per intent. Each binding contains only an
exact route id and nonzero generation. The embedded composition root creates
one private fixed `ExecutionService` before Kernel. The service exclusively
owns the Host-authoritative resource ledger, policy bindings, bounded ready
store, reserved-start transactions, physical routes, and completion callbacks;
Kernel injects that owner into each request-local `ComputeService`.

`ps::Host` is the public frontend-facing interface. The embedded Host adapter
copies public request/result values and uses the internal `InteractionService`
wrapper and `Kernel`; CLI/TUI code does not include or call those backend
facades directly. In the dirty-region context, Host exposes graph-scoped dirty
snapshot and lifecycle values, while `InteractionService` remains an internal
translation boundary rather than the authoritative source of dirty-region
generation or propagation.

Frontend compute commands build a public `ps::HostComputeRequest` rather than
passing positional boolean flags or internal request types through the public
seam. The embedded Host adapter translates that value to
`Kernel::ComputeRequest`. `Kernel` owns graph lookup, runtime start,
request-local quiet/skip-save policy, async scheduling, image extraction, and
LastError mapping. It then translates the internal request to
`ComputeService::Request`, which carries node target, cache, telemetry, intent,
dirty ROI, session identity, and explicit Run QoS data. Parallel/runtime
selection is carried separately as `ComputeService::ExecutionStrategy`. The
public `HostComputeExecutionOptions::maximum_parallelism` field carries one
optional positive Run concurrency ceiling through the adapter to that QoS.
The remaining identity and QoS values stay private descriptor inputs; the
plugin ABI is unchanged.

The dirty ROI remains a kernel-owned `PixelRect` while it is copied from
`HostComputeRequest` through `Kernel::ComputeRequest`, graph propagation,
planning, task selection, staged execution, and `NodeExecutor`. Extents use
`PixelSize`. No OpenCV geometry conversion occurs on this path; a provider may
create a local OpenCV rectangle or size only at an actual matrix or algorithm
call.

The CLI/REPL frontend is a permanent batch-oriented surface. It does not expose
RT intent commands, dirty ROI creation, or dirty source lifecycle commands such
as `compute rt`, `--dirty-roi`, `dirty begin`, `dirty update`, or `dirty end`.
`RealTimeUpdate` and dirty source lifecycle APIs are available through Host and
kernel/test callers, but the CLI does not expose them and must not be treated as
a production realtime control surface.

Fixed service threads are infrastructure rather than per-Run reservations.
Execution configuration resolves `0` to a bounded automatic value or preserves
an explicit `1..8`, freezes the CPU service count, starts one fixed CPU pool,
and starts one private Metal worker lane. The Metal lane is idle when the Host
does not expose Metal and is not a separately configurable worker grant.
Later zero/equal requests are idempotent; a conflicting positive request is
rejected. Before publishing work, each Run atomically reserves its complete
CPU, retained-memory, scratch, ready-entry, and ready-byte vector from the
service-owned Host ledger. Graph load copies route ids and generations but
owns no worker grant. This contract does not claim all threads used by compute,
operations, or a private GPU backend.

Benchmark configuration does not reconfigure that process pool. For each
benchmark Run, `execution.threads` resolves to an optional positive
`maximum_parallelism`: missing or `0` chooses a bounded automatic value in
`[1,8]`, while `1..8` selects an exact Run ceiling. One `BenchmarkService`
prepares execution at most once with automatic `worker_count=0`; this starts an
unconfigured pool or preserves an already fixed pool. `RunAll` ignores
disabled-session thread ranges after configuration parsing and does not execute
those sessions, logs and skips enabled sessions whose thread value is invalid,
and executes valid mixed caps against the same fixed pool.

`GraphTraversalService` is topology-only. It provides traversal order and
explicit upstream/downstream topology queries from `GraphModel` adjacency.
Dirty-region demand and ROI projection use `RoiPropagationService`, while
formal propagation extents come from `GraphExtentResolver`.

The current compute planning flow separates request-scoped static planning from
per-update dirty work selection:

```text
ComputeService facade
  -> GraphModel topology / GraphTraversalService queries
  -> FullTaskGraphExpander
  -> FullTaskGraph
  -> NodeCacheTaskGraphPruner
  -> ComputePlan / pruned ComputeTaskGraph
  -> DirtyRegionPlanner
  -> DirtyRegionSnapshot
  -> DirtySnapshotTaskGraphPruner
  -> DirtyUpdateWorkSet
  -> Run-owned TaskSubmissionPlan / ComputeTaskDispatcher
  -> ExecutionService policy / reserved start / private execution route
```

Before any of those planning steps, the coordinator has assigned the request a
canonical `(target, intent)` supersession key and checked graph-wide generation.
Each non-realtime HP service call creates exactly one request-owned
`ComputeRun`; a realtime call creates one `RunGroup` with separate HP and RT
child Runs. Each child captures a fresh opaque Run id, caller-visible session
label, non-reused strong Graph instance identity, nonzero authoritative
`GraphRevision`, target, single-domain intent, full or interactive quality,
explicit QoS, and the immutable supersession identity. Topology generation
remains a separate task-shape cache key and is not Run revision provenance.
Sequential, parallel, and dirty execution strategies share this boundary. Full HP Run control owns the
materialized plan and runner; every
real ready task retains a non-forgeable Run lease and
`(RunId, RunLocalTaskId)` through execution, dependency release, validation,
and commit. Full HP, connected-parameter preflight, and dirty HP/RT package
dependency-ready work as move-only `ReadyTaskSubmission` values for the fixed
multi-Run service. Planning freezes the selected operation implementation and
device before admission; each submission preserves that device through ready
storage and dependency release. Dirty phases use heap-owned contexts, so no
stack executor pointer crosses that boundary. Every private route retains the same Run lease
and Host-authored completion identity. Realtime children settle through
the request-owned `RunGroup`, whose stable control owns both observation leases,
the RT-first gate, cancellation fan-out, and deterministic aggregate outcome.

`FullTaskGraphExpander` expands the raw graph into the full node/tile task graph
for one compute domain. It does not depend on the request target, cache state,
or dirty snapshot. `NodeCacheTaskGraphPruner` then prunes that graph to the
request target/dependency cone and records cache availability for selected
nodes. Dirty updates add a separate `DirtySnapshotTaskGraphPruner` pass that
annotates the selected graph with dirty metadata and produces the active
`DirtyUpdateWorkSet`.

Single-threaded and parallel execution use the same full-expansion and
node/cache-pruning semantics. Their execution mechanisms differ, while graph
traversal, dirty planning, and task-graph pruning remain compute-owned.

`ComputePlan` is a static analysis for the current compute request and domain.
It is derived from the request-owned Graph snapshot captured at one exact
identity/revision and remains that request's topology contract. Every product
path writes only request-owned Graph/proxy state during operation work. Dirty
updates do not rebuild topology semantics. They use the current
`DirtyRegionSnapshot` and dirty ROI to activate or clip a
`DirtyUpdateWorkSet` from the plan for each HP or RT update queue.

The request plan must enumerate the real compute tasks available to the
request, including tile tasks when the selected implementation is tiled. Dirty
state only prunes or activates tasks from that enumerated graph. It must not
expand new tile tasks during dirty clipping; this keeps full-frame tiled
parallelism and dirty ROI execution on the same task model.

The request's `ComputeTaskGraph` is immutable while execution tasks derived
from it may still run. Each dirty compute request creates a generation-local
selection overlay from its plan and snapshot, then publishes only immutable ready submissions. A policy receives only
scalar frontier candidates and no replaceable task-graph object.

Execution granularity is a separate layer. A selected operation implementation
is node-wide, monolithic, or tiled. The current full-task population maps
`MICRO` metadata to 16-pixel tasks, `MACRO` metadata to 256-pixel tasks, and an
unspecified preference to 128-pixel tasks. Edge tasks are clipped to the output
extent.

Dirty snapshot grids are different metadata: HP snapshots materialize Micro
keys on a 64-pixel HP grid and RT snapshots materialize Micro keys on a
16-pixel proxy grid. The dirty selector intersects those records with task
shapes already present in the full graph. It does not create `ReTileTask`, RT
Macro_64 records, or dynamic Micro/Macro conversion. HP and RT plans remain
single-domain; realtime intent coordinates separate sibling work without
creating cross-domain task dependencies.

## Compute Intents

The kernel recognizes two formal compute intents:

| Intent | Meaning |
| --- | --- |
| `GlobalHighPrecision` | Full-quality HP compute. Owns high-precision output. Non-realtime compute enables only this HP path. |
| `RealTimeUpdate` | Interactive realtime update. Requires a dirty ROI and enables the HP/RT dual path. |

The intent model is formal. `ComputeService` remains the compute facade and
planning boundary. Private route metadata selects the physical path. The `cpu`
and `serial_debug` routes expose CPU only. `gpu_pipeline` exposes Metal then CPU
when the Host reports Metal, and CPU only otherwise. CPU, serial-debug, GPU,
connected-parameter preflight, full, and dirty phases all use the fixed
injected service; GraphRuntime stores only copied route ids and generations.

HP/RT dual path semantics belong to realtime intent, not to the parallel
execution mode. In realtime mode, HP computes the full-size authoritative node
work while RT computes the downscaled proxy, currently one quarter of width and
height, or one sixteenth of the pixel count. `IntentUpdateCoordinator` launches
two sibling calls, and each sibling resolves its copied private route binding through the process service. `ComputeIntent` does not itself specify
QoS or final physical policy. The service consumes explicit `ComputeRunQos`;
current Kernel-created Runs use the descriptor default of throughput unless a
private caller explicitly supplies interactive QoS. Intent, quality, and
maximum parallelism never infer class, deadline, or weight.

The current collaborator responsibilities and non-ownership boundaries are
documented in `Compute-Boundaries.md`.

## Sequential Compute

Sequential compute uses recursive dependency resolution:

1. Validate the target node.
2. Resolve traversal order and optionally clear caches.
3. For each dependency, compute upstream nodes recursively.
4. Copy the static `ParameterMap` into `runtime_parameters` and overlay
   connected named `ParameterValue` outputs without format conversion.
5. Resolve an operation implementation for HP intent.
6. Execute monolithic or tiled operation.
7. Store output, emit events, and update timing in the request snapshot. Disk
   writes stay suppressed until the final exact-revision commit transaction.

Sequential compute is useful for simple execution and debugging. It creates
the same internal full-expansion and node/cache-pruned task graph semantics as
the parallel path before executing the recursive path.

## Parallel Compute

Parallel compute derives a `ComputePlan` by expanding the full task graph and
then pruning it with `NodeCacheTaskGraphPruner` from `topo_postorder_from`.
`ComputeDispatchPlanBuilder` records that cache-pruned plan for inspection.
The request `ComputeRun` owns the `TaskSubmissionPlan` that materializes the
plan's `ComputeTaskGraph` into dependency counters, ready values, operation
variants, selected devices, and temporary result slots. The dispatcher creates immutable, lease-backed `ReadyTaskSubmission` values
and submits one initial ready batch to the injected `ExecutionService`;
dependent completion creates further submissions through the same active Run
and bounded store. Tiled operations may spawn micro-tasks and retire the
selected private route's logical completion count.

Before a Run is published, the service atomically reserves its complete
checked CPU, retained-memory, scratch, ready-entry, and ready-byte vector.
CPU slots and uniform per-task retained/scratch envelopes use the minimum of
fixed workers, logical tasks, and the Run's optional positive maximum
parallelism; ready entries and bytes still cover every logical task.
Initial and dependent entries hold child ready grants; reserved start exchanges
that authority for CPU/memory/scratch before entering the submission's fixed
CPU or Metal lane. The service rejects a device outside the configured
route/Host inventory before Run publication. Failure, queue purge, and
successful settlement release exact capacity. The fixed lanes never resize
per Run, and CPU/Metal callbacks share the Run's admitted parallelism ceiling.
Execution inspection and replacement share the per-graph
compute-request lane with compute, so copied route generations remain coherent.
Replacement validates one closed-vocabulary route and publishes a new nonzero
generation in one transaction; failure preserves the old binding.

The ready-store policy charges each dispatch
`work_units + ceil(complete_ready_grant_bytes / 4096)`: Graph fairness uses raw
cost in one accumulator per selected service class, while each immutable-class
Run uses `ceil(cost / weight)`. Interactive work prefers an earlier present
monotonic deadline; throughput ordering is weighted and deterministic. The
store first selects the service class, forcing Throughput after at most three
Interactive dispatches while both classes remain ready. Eight-dispatch aging
then applies only within that selected class and cannot change it.

Configured interactive headroom limits only active built-in Throughput root
reservations to the general ceiling. Interactive work does not consume that class quota, but the sole ledger still authorizes
their shared physical capacity. A Throughput charge is committed atomically
with its ledger reservation and is removed only when the root reservation is
physically released after all child grants. Initial and dependent submissions
use the same route, and the service retains each Run fairness row across
temporary emptiness. Policy strategies own no worker, token, budget, Run, or
Graph; the service and ledger remain the physical and resource authorities.

`ComputeTaskDispatcher` keeps plan execution, dependency accounting, sparse
node-id mapping, temporary-result indexing, event logging, exception
propagation, and final target selection inside the compute-service boundary.
The Run owns the corresponding plan and result-slot storage. The dispatcher
uses the private execution task runtime for already-planned work; it does not make
a policy or route own dirty propagation, compute-task derivation, or the task graph
itself. If the pruned planned dispatch is empty while the target has no reusable
HP output, the dispatcher reports a planning contract error instead of falling
back to recursive sequential compute.

For dirty execution, `DirtySnapshotTaskGraphPruner` materializes only the active
`DirtyUpdateWorkSet` selected from the request's `ComputeTaskGraph`. Runtime
dependency counters and ready-task queues are execution artifacts; they are not
stored in `DirtyRegionSnapshot` and are not owned by a policy or route.

Explicit Host begin/update/end calls identify a dirty source node and update
graph-scoped lifecycle facts; they do not trigger compute. The current backend
has no node subscription, automatic request launch, or active-request
coalescing contract.

Lifecycle updates enter the per-graph serialized path and call
`DirtyControlLane`. The lane updates source state, rebuilds source-local derived
records for the event domain, and returns wakeup/cutoff hints in
`DirtyControlLaneResult`. This lifecycle path does not traverse downstream
edges, and the hints currently have no production compute consumer. The
embedded Host exposes copied inspection values, not internal control fields.
The execution service receives only immutable ready submissions with
Host-authored identity, route, and priority metadata; dirty generation is not
reused as a route or policy generation.

## Graph-State Access and Commit Policy

Graph-state operations such as YAML loading, cache commands, inspection, and
ROI projection are operations on the visible `GraphModel`. They are not
compute-task dispatch and are not routed through the private execution task runtime.

The visible-state default is per-graph exclusive access through
`GraphStateExecutor`. Structural/cache/dirty mutations, request snapshot
capture, exact commit predicate evaluation, and no-throw publication use this
lane. Long-running planning, operation callbacks, policy calls, and route waits use only
request-owned `GraphModel` and optional `RealtimeProxyGraph` snapshots outside
it. A mutation can therefore advance `GraphRevision` while an operation is
blocked; that request later fails its exact revision predicate instead of
overwriting the newer Graph.

Each runtime also owns a bounded serial compute-request lane. Synchronous,
image-returning, and asynchronous compute publish through the coordinator;
execution inspection and replacement use ordinary one-shot submissions.
The lane charges exactly 64 total queued, running, or parked units. Each
supersession key reserves one persistent continuation ticket and one latest
pending mailbox; repeated publication replaces only that pending value. The
existing lane worker is the sole logical active-request runner, and each ticket
turn executes at most one generation through the existing Kernel/ComputeService
path. It creates no extra background runner or generation thread. Unrelated
Graph runtimes and equal labels in distinct live Graph instances remain
independent.

Required-session lifetime admission still covers synchronous and asynchronous
compute, required graph save, node-YAML replacement, ROI projection, timing
inspection, and all-cache clearing through public result/status translation.
During embedded close, Host first publishes its close marker and lets earlier
synchronous admissions finish public translation. Kernel then stops
compute-request admission before Host waits for async placeholders and status
publication. This wakes a producer blocked by the full request FIFO without
requiring queue space. Accepted requests drain while graph-state admission
remains open for their final commit; Kernel then drains graph-state, marks the runtime stopped, and removes it.
Process-owned workers and policy bindings outlive the Graph; copied route
bindings have no physical owner to stop. Node replacement and ROI projection
still perform required lookup and mutation in one graph-state work item.
Execution information copies route/statistics inside the compute-request lane;
no physical owner or queue capability crosses it.

Every product compute path now uses staged output. Kernel captures the complete
Graph and optional RT proxy at one identity/revision, suppresses snapshot disk
writes, and executes sequential, policy-selected, dirty HP, and RT work only
against those snapshots. After local output validation, ComputeService advances
the matching Run to `CommitPending`. A private product commit policy validates
the exact Run/staged/live identity, authoritative revision, and current
supersession key/generation, performs eligible deferred HP cache persistence,
and swaps complete visible state in the same graph-state work item. It publishes
Run success only after that transaction succeeds.

This is the current baseline through issue #76. A private request source can
cooperatively cancel one HP Run or both current realtime child Runs; immutable
deadlines propose `DeadlineExceeded` at bounded observation points, and the
Run-owned terminal arbiter orders cancellation, failure, and visible commit.
Per-Graph latest-wins publication makes the newest generation authoritative for
that exact key, requests cancellation of an older active owner, coalesces one
pending owner, and still denies stale commit if cancellation arrives late.
The process-owned `RunLifecycleRegistry` now begins a candidate before capture
or planning, retains a Graph lifetime lease, and atomically installs either one
standalone Run or both realtime children as a complete bundle. Empty/no-op and
connected-preflight paths use the same admission/finalization boundary. Graph
close and process shutdown cancel through that registry and wait for exact
physical/resource settlement before unregistration. This remains cooperative
rather than preemptive execution; provider preemption and public Host/CLI/IPC
cancellation are not claimed.
Commit policy remains conceptually separate from `ComputeIntent`, because
HP/RT intent semantics define neither visibility nor cancellation authority.

## GlobalHighPrecision

`GlobalHighPrecision` is the full-quality path. Without a dirty ROI it performs
normal full compute. With a dirty ROI it enters the HP dirty update path; it
does not unconditionally replace that request with full-frame recompute.

HP dirty-region update is a first-class dirty-ROI consumer, not just a full
recompute fallback. Normal dirty ROI requests compute a backward ROI plan, align
dirty regions to HP tile boundaries, clip the HP work set from the request's
`ComputeTaskGraph`, update affected HP tiles, record HP ROI/version metadata,
and can schedule downsample work to refresh `RealtimeProxyGraph` state.
`IntentUpdateCoordinator` routes global HP dirty requests to this path and
records `intent_coordinator_global_dirty_update`.

Forced HP dirty updates are the exception: when `force_recache=true`, the HP
staging buffer intentionally does not seed pixels from the previous HP cache, so
the executor expands the HP planning ROI to the target node's full current HP
extent before committing. This preserves complete authoritative HP output while
keeping non-forced dirty ROI requests local.

Dirty-region state planning runs through the graph-scoped
`DirtyRegionPlanner`, and the resulting `DirtyRegionSnapshot` feeds dirty
work-set materialization and interaction-facing inspection summaries.

Consequently, one public Host HP dirty request exercises one continuous
kernel-native geometry path: request validation, graph-scoped backward
projection, immutable plan selection, source-first ready dispatch, node
execution, and staged HP commit all observe `PixelRect`/`PixelSize` values.

## RealTimeUpdate

`RealTimeUpdate` requires a dirty ROI. A request without `dirty_roi` is invalid
and returns a clear public `ps::Host` status/error value. The embedded adapter
derives that value from internal Kernel and InteractionService diagnostics. The
request does not implicitly mean full-frame RT update.

With a valid dirty ROI, realtime compute enables both paths. RT is launched
first and updates a low-resolution `RealtimeProxyGraph`; HP updates the
full-size authoritative output for the affected graph work through a staged
buffer. If the request is forced, the HP sibling follows the same full-frame HP
planning rule as Global HP dirty update, while RT proxy work remains scoped to
the RT dirty plan. The request creates one `RunGroup` containing an HP child Run
with full quality and an RT child Run with interactive quality. When both
physical execution domains
are available, `IntentUpdateCoordinator` starts both siblings concurrently,
waits for RT first, and uses a sibling commit gate so HP can attempt its
independent Graph commit only after revision-validated RT proxy publication
succeeds. Built-in CPU children share the fixed
service and their copied private routes. Without parallel domains,
the same callbacks run inline in RT-then-HP order. Each child publishes its own
terminal outcome only after its visible commit, and the group resolves a stable
aggregate only after both child observation leases settle. A Graph mutation
after valid RT publication may make the HP sibling stale; RT remains visible
while HP fails with `ComputeError`. A newer realtime generation cancels both
older children and denies an older pending gate. If the old RT proxy committed
first it remains visible, but the old HP sibling still fails the
current-generation predicate.

The concurrent path also shares one request-owned per-node synchronization
object between the siblings. It protects live `Node` snapshot and
format-neutral parameter resolution for the same node without merging the two
domain plans:
different nodes and the operation bodies remain concurrent, and the object is
released after both sibling futures have drained, including failure cleanup.

Realtime planning is intentionally per path, not a single mixed-domain planner
call. `IntentUpdateCoordinator` dispatches sibling HP and RT update callbacks
and records RT-first/concurrent stages for Dirty RT requests. Each path uses a
single-domain request plan and a same-domain dirty snapshot: the HP callback
uses a `GlobalHighPrecision` node/cache-pruned plan with an HP dirty snapshot,
and the RT callback uses a `RealTimeUpdate` node/cache-pruned plan with an RT
dirty snapshot. HP dirty node execution writes into
`HighPrecisionDirtyWriteBuffer`; RT dirty node execution writes into
`RealtimeProxyWriteBuffer` and commits only to `RealtimeProxyGraph`. The dirty
snapshot clips or activates the update work set from the path's task graph.
This keeps full task expansion, node/cache pruning, dirty snapshot pruning, and
output commit as separate contracts for each compute domain.

The passed dirty ROI is converted into graph-scoped planner state for the
current request. Public `ps::Host` begin/update/end methods translate through the
embedded adapter to internal `Kernel` / `InteractionService` dirty-source
lifecycle methods, so frontend or node-facing code writes state through the same
graph-owned boundary. The Host lifecycle call is currently the public write
surface; there is no public node-event subscription that automatically creates
or schedules a compute request.

RT task graph expansion is domain-aware. When an operation has distinct HP and
RT metadata, the `RealTimeUpdate` plan uses RT metadata for tile size and
dependency ROI planning, while the HP sibling uses HP metadata. This keeps RT
Micro_16 planning independent from HP Macro_256 throughput defaults.

The current implementation has no node-to-backend realtime event subscription.
Nodes and Host lifecycle calls may update graph-owned dirty state, while
`InteractionService` remains only an internal translation boundary and the CLI
remains batch-oriented. Any subscription surface is outside the current
software contract.

Current defaults:

| Parameter | Current value | Status |
| --- | --- | --- |
| RT downscale factor | `4` | Tunable implementation default. |
| RT micro tile size | `16` | Tunable implementation default. |
| RT macro tile size | `64` | Tunable implementation default; same numeric size as HP micro, different domain. |
| HP micro tile size | `64` | Tunable implementation default. |
| HP macro tile size | `256` | Tunable implementation default. |

These constants are not permanent ABI.

## Events and Timing

`GraphEventService` publishes per-node compute events into a thread-safe,
fixed-capacity ring. The production capacity is 8,192 events per graph. Each
accepted publication receives a monotonically increasing unsigned 64-bit
sequence in `1..UINT64_MAX-1`; `UINT64_MAX` is an exhaustion sentinel and is
never assigned to an event. The ring evicts exactly its oldest retained event
when full and accounts for that eviction with a saturating drop counter.

Event names and sources are measured with `std::string::size()` before they are
copied into retained storage, so the public 1,024-byte bound is a UTF-8 byte
bound. If either field exceeds that bound, the complete publication is dropped
without truncation. The attempt still consumes one valid sequence and adds one
drop. After sequence exhaustion, each publication attempt adds only the single
exhaustion drop and cannot wrap either the sequence or drop counter.

`Host::drain_compute_events(session, limit)` accepts limits from 1 through
1,024 and returns `ComputeEventBatch`: sequenced `events`, `next_sequence`,
`has_more`, and `dropped_count`. A successful call removes only the returned
oldest page and atomically resets the shared drop counter, including for an
empty page. Invalid limits return `GraphErrc::InvalidParameter` before any
removal or reset. The event service reserves result capacity before moving an
event, so `std::bad_alloc` cannot remove an event that the caller did not
receive. CLI consumers repeatedly request the maximum bounded page only while
`has_more` is true, but one polling pass has a fixed eight-page budget derived
as `ceil(8192 / 1024)`. This covers a complete retained production ring when no
producer races the pass, prevents a live producer from extending one pass
indefinitely, and leaves deferred or newly published events for a future poll.
No unbounded Host vector drain exists.

`TimingCollector` separately stores node timings and total elapsed compute time
when timing is enabled. Debug metadata in `NodeOutput` records worker id,
timestamp, execution time, device, and optional range checks.

## Error Handling

Compute failures throw `GraphError` with `GraphErrc` categories where possible.
Synchronous Kernel paths store a mutex-protected per-graph `LastError` for
best-effort observation. An asynchronous work item instead returns its own
`AsyncComputeResult` containing the exact failure code/message and only mirrors
that value into `LastError`; its Host future never reconstructs status from the
mutable mirror. The embedded adapter maps these values to public
`OperationStatus`, `Result<T>`, or `ps::Host::last_error()` values. Frontends
observe only the public Host surface and never inspect Kernel or
`InteractionService` directly.

Policy and execution configuration/replacement failures occur before publishing a candidate. An invalid above-eight request, conflicting positive
fixed-pool request, or unknown type maps to `InvalidParameter`; exhaustion of
the Host ledger preserves `GraphErrc::ComputeError` through embedded Host and
IPC status boundaries. Built-in CPU Run aggregation overflow or full-vector
exhaustion likewise returns `ComputeError` before any initial ready entry is
published. A present zero public `maximum_parallelism` is invalid and is
rejected at Host or IPC decoding before graph execution.

## Boundaries and Rationale

- One request plan supplies both sequential and parallel execution semantics;
  the execution strategy changes mechanics, not topology or dirty meaning.
- One non-realtime HP request owns one `ComputeRun`; realtime owns one
  `RunGroup` with separate HP and RT child Runs from pre-planning descriptor
  capture through independent terminal publication and aggregate settlement.
  HP Runs own full-plan temporary results or dirty HP
  staging. Every built-in CPU callback retains a matching child lease and
  composite task identity, but a Run does not own Graph state, workers, or the
  meaning of dependency transitions.
- `GraphStateExecutor` protects visible graph coherence and exact commit, while
  the private compute-request lane protects same-Graph request and route
  replacement ordering. The private execution task runtime receives only ready
  compute work; graph-state commands therefore never become execution tasks.
- HP cache and RT proxy state use separate staged commit paths, so preview
  state cannot become authoritative HP output by implication.
- Execution entry versions reject stale queued submissions only and are not Run
  identity. HP/RT failures route through
  `(RunId, RunLocalTaskId)` under a stable lease. Current Runs record explicit
  QoS that the service applies to ordering, fairness, and headroom admission,
  plus exact Graph identity/revision provenance. A deadline remains an ordering
  input and is also an expiry trigger when a Run reaches a cooperative
  observation point. Built-in ready publication, queue/dequeue, operation,
  dependency, phase, and commit boundaries observe private Run cancellation;
  entered non-preemptible providers may finish, but their staged output cannot
  commit. The current supersession generation is an independent commit
  predicate, so late cancellation cannot resurrect stale output. Graph-close
  and process-shutdown lifecycle cancellation are current private contracts;
  public cancellation control remains future.

These separations keep planning, physical dispatch, and visible commit
independently testable. [ADR 0001](../adr/0001-graph-state-access-is-not-scheduler-dispatch.md)
governs the current graph-state/dispatch distinction. The accepted
[ADR 0007](../adr/0007-compute-runs-and-process-execution-have-separate-owners.md)
defines the current fixed multi-Graph HP/RT service, independent child Runs,
built-in policy ordering, lease/completion isolation, authoritative
revision/generation-safe staging, RT-first independent commit gate,
cooperative Run cancellation, deterministic `RunGroup` settlement, and
latest-wins supersession, admitted-Run registry, Graph lifetime leases, and
close/shutdown lifecycle ownership. The
[process execution domain target](../roadmap/Kernel-Evolution.md#process-execution-domain)
retains the durable ownership direction without changing these current facts.

## Implementation and Validation Entry Points

- `src/lib/runtime/kernel_compute.cpp`
- `src/lib/host/embedded_host.cpp`
- `src/lib/benchmark/benchmark_service.*`
- `src/lib/ipc/request_router.cpp`
- `src/lib/compute/compute_service.*`
- `src/lib/compute/run_lifecycle_registry.*`
- `src/lib/compute/execution_lifecycle_telemetry.*`
- `src/lib/compute/compute_supersession.*`
- `src/lib/compute/compute_request_coordinator.*`
- `src/lib/compute/compute_run.*`
- `src/lib/compute/run_group.*`
- `src/lib/compute/compute_dispatch_plan_builder.*`
- `src/lib/compute/compute_task_dispatcher.*`
- `src/lib/compute/intent_update_coordinator.*`
- `src/lib/compute/dirty_update_executor.*`
- `src/lib/runtime/graph_event_service.*`
- `tests/integration/test_compute_service_split.cpp`
- `tests/unit/test_policy_registry.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_host_adapter.cpp`
- `tests/integration/test_opencv_operation_concurrency.cpp`
- `tests/unit/test_ipc_protocol.cpp`
- `tests/unit/test_compute_run.cpp`
- `tests/unit/test_event_stream_boundaries.cpp`
