# Kernel Evolution Target

## Status and Scope

This document records the accepted post-merge architecture direction. It is a
target, not a description of current software behavior and not an
implementation checklist. Current facts remain authoritative in
`docs/kernel-architecture/`; architectural decisions are recorded in
`docs/adr/`; implementation state is tracked only in the linked GitHub
Projects and Issues.

[ADR 0006](../adr/0006-kernel-documentation-separates-facts-decisions-targets-and-status.md)
defines this separation and the promotion workflow. Each delivery slice cites
its current-state baseline, governing ADR, exact target section, live
Project/Issue state, and actual verification. Completing a delivery item does
not by itself make the target current behavior; the corresponding maintained
architecture document changes only when implementation and durable tests
support it.

The current branch is treated as a local, single-user, embedded or Unix-socket
sidecar baseline. The target described here is required before Photospider is
presented as a general dataflow kernel, a low-latency interactive engine, or a
multi-session server runtime.

## Development Domains

| Domain | GitHub Project | Parent Issue | Target outcome |
| --- | --- | --- | --- |
| Dependency-neutral kernel | [kernel-dependency-decoupling](https://github.com/users/kevin-zf1123/projects/2) | [#51](https://github.com/kevin-zf1123/photospider/issues/51) | Kernel geometry, values, buffers, graph documents, and cache behavior do not use OpenCV or YAML as their semantic language. |
| Run and process execution domain | [compute-run-execution-domain](https://github.com/users/kevin-zf1123/projects/3) | [#64](https://github.com/kevin-zf1123/photospider/issues/64) | Request-owned `ComputeRun`, process-owned CPU execution, resource accounting, graph revisions, cancellation, and supersession. |
| General data and heterogeneous execution | [generic-data-heterogeneous-execution](https://github.com/users/kevin-zf1123/projects/4) | [#77](https://github.com/kevin-zf1123/photospider/issues/77) | `Value`, `DataDescriptor`, `BufferHandle`, `Region`, device queues, fences, transfers, and bounded compute I/O. |
| Execution profiles and secure services | [execution-profiles-server-isolation](https://github.com/users/kevin-zf1123/projects/5) | [#91](https://github.com/kevin-zf1123/photospider/issues/91) | Interactive and throughput profiles, an independent server control plane, constrained workers, and isolated plugin execution. |

The merge gates for the current refactor remain in
[codebase-refactor](https://github.com/users/kevin-zf1123/projects/1), aggregated
by [issue #42](https://github.com/kevin-zf1123/photospider/issues/42).

### Current containment baseline

[Issue #43](https://github.com/kevin-zf1123/photospider/issues/43) established
the initial scheduler-worker containment and
[Issue #44](https://github.com/kevin-zf1123/photospider/issues/44) established
the bounded graph-state lane. Issues #69 through #75 have since replaced the
worker-only containment and the worker-owning scheduler SDK with one
Host-composed execution domain, atomic resource vectors, policy-aware bounded
ready storage, revision-safe staged publication, pure-C policy generation, and
Host-private execution routes. Visible compute captures complete request-owned
state and executes outside graph-state; a second bounded serial lane preserves
same-Graph request ordering without owning a physical executor. The current
bounded contract:

- gives every embedded Host one shared CPU service and one ledger whose default
  CPU dimension is 32, and admits every Run with a complete checked CPU,
  retained-memory, scratch, ready-entry, and ready-byte vector;
- sends initial and dependent work through the same entry/byte-bounded ready
  store and exchanges ready authority for execution grants only at reserved
  start;
- maintains exactly one Interactive and one Throughput policy binding, applies
  Host-authored class/frontier/fairness rules, and validates every built-in or
  DSO decision against the immutable original snapshot and current Host state;
- exposes policy plugins through the self-contained C11 ABI v1, while keeping
  workers, queues, resources, Run/Graph state, and completion routes absent
  from the ABI;
- makes the first invalid plugin decision sticky for its exact binding
  generation and falls back through the same trusted built-in selection path;
- keeps `cpu`, `serial_debug`, and `gpu_pipeline` as closed private
  execution-route ids; a Graph stores only copied route ids and generations,
  never a physical worker, queue, plugin context, or policy binding;
- bills starts by work plus ready-byte quanta, maintains hierarchical
  Graph/Run fairness, ages ready work, reserves interactive headroom, and
  guarantees Throughput progress after a bounded Interactive burst;
- replaces graph-state async-per-submit with one worker and a 64-waiting-task
  FIFO per Graph, applying blocking backpressure without dropping admitted
  work, and gives every Graph a second lane with the same bound for request
  serialization;
- assigns every live Graph a non-reused strong identity and checked nonzero
  revision, and publishes product snapshots only after exact equality; and
- makes embedded close publish its Host marker, drain pre-marker synchronous
  admissions, stop compute-request admission before waiting for async
  placeholders, drain compute-request work while graph-state remains
  available, and then drain graph-state without tearing down a process-owned
  execution route.

The default 32 CPU slots cover admitted Run execution grants. Fixed
`ExecutionService` threads and its private route machinery are infrastructure.
The ledger does not count graph-state or compute-request executors, which each
have a separate one-worker-per-Graph bound; nor does it claim
operation-internal threads, daemon/frontend workers, all OS threads, or
undeclared device/I/O/plugin-process resources. Issue #70 replaces the former
worker-only counter completely:
`ExecutionService` now owns the sole Host-authoritative ledger, admits each
built-in CPU Run with one checked full-vector reservation before publication,
and requires initial and dependent work to hold entry/byte grants while stored
in its bounded ready queue. Issue #71 adds the current private interactive and
throughput strategies, explicit QoS ordering, work/byte charging, Graph/Run
fairness, aging, headroom, and bounded throughput progress. Issue #72 adds
strong Graph identity/revision, request-owned product snapshots, exact-revision
commit, and RT-first independent child publication. Issue #73 adds private
cooperative cancellation, monotonic deadline observation, exact queued/running
drainage, and cancellation/commit arbitration. Issue #74 adds request-level
realtime `RunGroup`, checked per-Graph latest-wins generations, bounded
ticket-backed coalescing, and current-generation commit authority. Issue #75
removes the scheduler SDK and adds pure-C policy ABI v1, atomic policy binding
replacement, Host-authored frontier and fallback, sticky generation-local
faults, reserved start, and private execution routes. Registry-owned
close/shutdown cancellation, exact settlement, and lifecycle telemetry are
current Issue #76 behavior.
[ADR 0007](../adr/0007-compute-runs-and-process-execution-have-separate-owners.md)
is authoritative for the detailed current Run lifetime, owner boundaries,
resource mint, close/shutdown scope, and delivery dependencies.

## Architectural Principles

1. `ps::Host` remains the only product seam outside the backend.
2. Graph-state operations never enter the ready store, policy selection, or an
   execution route as compute work.
3. Compute planning owns topology, dependency, ROI, dirty selection, and ready
   detection; scheduling sees only immutable metadata for concrete ready work.
4. Semantic intent, resource policy, and commit visibility remain separate.
5. Physical CPU, GPU, I/O, and external-process resources have one explicit
   process owner and a host-authoritative budget.
6. External libraries and document formats enter through adapters; their types
   do not define kernel geometry, values, planning, or cache semantics.
7. Data descriptors, ownership, device synchronization, and regions are
   explicit. No representation relies on an opaque context to recover facts
   required for correctness.
8. Local sidecar, server control plane, worker runtime, and untrusted plugin
   execution are separate security domains.

## Target Ownership Structure

```mermaid
flowchart TD
  HOST["Host / Kernel"] --> CAPTURE["Graph-state lane: capture revision"]
  CAPTURE --> REV["immutable GraphRevision"]
  REV --> SERVICE["ComputeService"]
  SERVICE --> RUN["ComputeRun"]
  SERVICE --> PLAN["ComputeTaskPlanner"]
  PLAN --> GRAPH["ComputePlan / ComputeTaskGraph"]
  GRAPH --> DISPATCH["ComputeTaskDispatcher"]
  RUN --> DISPATCH
  DISPATCH -->|"ReadyTaskSubmission"| EXEC

  subgraph EXEC["Process-owned ExecutionService"]
    ADMIT["AdmissionController"] --> LEDGER["ResourceLedger"]
    LEDGER --> READY["Host-owned ReadyTaskStore"]
    READY --> POLICY["Policy binding / pure-C policy ABI"]
    POLICY --> ROUTER["Resource router"]
    ROUTER --> CPU["CPU executor"]
    ROUTER --> DEVICE["DeviceExecutorRegistry"]
    ROUTER --> IO["Compute I/O executor"]
    ROUTER --> PINVOKE["PluginInvocationExecutor"]
  end

  PINVOKE --> PSUP["PluginRuntimeSupervisor"]
  CPU --> COMPLETE["TaskCompletion"]
  DEVICE --> COMPLETE
  IO --> COMPLETE
  PINVOKE --> COMPLETE
  COMPLETE --> RUN
  COMPLETE --> DISPATCH
  RUN --> COMMIT["ComputeCommitPolicy"]
  COMMIT --> VALIDATE["Graph-state lane: validate and publish"]
  VALIDATE --> VISIBLE["GraphModel / RealtimeProxyGraph"]
```

`Process-owned` means one explicit owner in the product composition root. It
does not mean a static singleton. Embedded tests, the desktop product, and a
worker process must be able to construct, inject, and destroy an execution
domain deterministically.

The graph-state lane now captures an immutable revision and later validates
the commit predicate. Long-running planning and execution occur outside the
exclusive `GraphModel` mutation boundary, so one `ComputeRun` does not prevent
the frontend from producing a newer revision. Issue #72 makes that minimum
identity/revision staging behavior current, Issue #73 makes private Run
cancellation and commit arbitration current, and Issue #74 makes request-level
grouping plus supersession generation current. Issue #75 makes policy
generation, reserved start, and private execution routes current. Issue #76
makes lifecycle registry, close/shutdown, and telemetry current. The diagram
still includes later device, I/O, and isolated-plugin target slices.

## Run and Process Execution Domain Contract

[ADR 0007](../adr/0007-compute-runs-and-process-execution-have-separate-owners.md)
refines the high-level direction from ADR 0003. This section summarizes its
durable target constraints; the ADR is authoritative when a summary omits
detail.

### `ComputeRun`

The current baseline through Issue #75 implements exactly one private Run
around every non-realtime HP service call. A realtime call instead creates one
request-owned `RunGroup` with separate HP `Full` and RT `Interactive` child
Runs. Each Run captures a process-lifetime opaque id, session label,
strong Graph instance identity, authoritative revision, target, intent,
quality, and explicit QoS;
owns monotonic phase and exact-once terminal state; and owns the corresponding
full submission plan/temporary results or standalone dirty staging through
shared control. Full HP work retains stable non-forgeable leases, owns its
runner, and routes task failure only through matching
`(RunId, RunLocalTaskId)`. Built-in HP and RT CPU ready work, including dirty
and preflight work, now crosses the injected multi-Run `ExecutionService`
boundary as move-only submissions with heap-owned callback context. The
service obtains one checked full-vector reservation for each Run before
publication. Initial and dependent ready work must hold bounded-store grants,
which workers exchange for execution grants. Product compute uses complete
request-owned Graph/proxy snapshots and publishes only after the issue #72
exact-revision predicate succeeds.
Explicit QoS class, deadline, and weight enter the current built-in policy
route; intent and quality do not infer them. Issue #73 observes each immutable
deadline and private request source at bounded planning, queue, callback,
dependency, phase, and commit boundaries. Issue #74 adds each request's
immutable supersession key/generation, request-wide realtime cancellation and
aggregate settlement, one pending mailbox and persistent ticket per exact key,
and current-generation commit validation. Issue #75 adds Host-authored policy
frontiers, generation-scoped policy binding and fault state, reserved-start
admission, and private execution routes. Issue #76 adds current lifecycle
registry wiring, Graph close, process shutdown, and telemetry. Public
cancellation control remains a subsequent slice.

The remainder of this section describes the implemented ownership contract and
its remaining target extensions.

`ComputeRun` is the unit of compute identity and lifetime. It is distinct from
`GraphRuntime`, a policy selection snapshot, and `ComputeIntent`.

A non-realtime HP request owns one Run. A request coordinating independently
planned HP and RT siblings owns a request/run-group identity plus one child Run
per domain. Group identity coordinates caller-visible completion but does not
create cross-domain task dependencies.

The request-owned `RunGroup` succeeds only when both children succeed and then
returns the RT child output. Its control block owns child observation leases,
the sibling gate, aggregate arbiter, and caller promise, not either child plan,
dispatcher, staged output, or reservation. Its deterministic aggregate order
is failure, cancellation, then success; resource exhaustion outranks another
failure, RT outranks HP within the same failure class, and a group-origin
cancellation outranks a child-only reason; the first group-origin reason
accepted by the monotonic group arbiter remains stable, followed by the RT/HP
child tie-break. Group/lifecycle cancellation reaches both children. RT failure
or cancellation before RT commit
permanently denies HP commit and requests HP cancellation; HP failure or
cancellation does not roll back an already-published RT proxy or request RT
cancellation, but prevents group success. Caller completion waits for both child
Runs to become terminal, quiescent, and finalized, for both admission attempts
to resolve, for exact graph/resource release, and for every installed registry
entry to unregister. The ready caller future contains only a copied aggregate
value, not a child `RunLease`.

A Run owns or captures:

- one opaque, non-reused `RunId` and optional request/parent/run-group identity;
- immutable graph identity, `GraphRevision`, target, and request input snapshot;
- single-domain `ComputeIntent`, quality, QoS, monotonic deadline, weight, and
  maximum parallelism;
- supersession key and generation;
- monotonic cancellation state and one terminal outcome;
- stable storage for the request plan, dispatcher dependency state, staged
  outputs, and exception state, kept alive through Run leases;
- resource reservations and commit policy.

Tasks are addressed by `(RunId, RunLocalTaskId)`. A local task id has no meaning
outside its Run.

The target phase progression is:

```text
Created -> Admitted -> Queued -> Running -> CommitPending -> Terminal
```

Safe paths may skip nonterminal phases but never move backward. Exactly one
`Succeeded`, `Failed`, or `Cancelled` outcome is published. Completion alone is
not success: dependency aggregation and the serialized graph-state commit
predicate must succeed. Cancellation, Run-internal failure, and the Graph/RT
result-commit contender share one Run terminal arbiter. Terminal publication
may precede physical quiescence when non-preemptible work must drain.

`ComputeRun` gives request-local state a stable lifetime. It does not own the
meaning of dependency transitions: `ComputeTaskDispatcher` remains responsible
for dependency counters, ready detection, and dependent release.

`ComputeIntent` describes HP/RT business semantics. QoS and deadline describe
resource policy. `ComputeCommitPolicy` decides whether a completed result may
become visible. None may be inferred from another.

### Run leases, ready tasks, and completion

Every accepted ready-store entry, executing callback, completion record,
dispatcher continuation, and commit continuation owns or transfers one
non-forgeable `RunLease`. The lease keeps the plan, dispatcher, temporary/staged
output, exception state, and completion endpoint alive without transferring
Graph or resource authority.

Only dispatcher-ready `ReadyTaskSubmission` values enter the execution domain.
They carry immutable metadata, `(RunId, RunLocalTaskId)`, stable executable
state, resource requirements, and a Run lease. They never carry `GraphModel`, a
plan/task graph, dirty state, dependency maps, cache authority, or visible
commit authority.

Completion returns through the lease to the matching Run dispatcher. Newly
ready dependents re-enter process admission, the bounded ready store, and global
policy. Different Runs may reuse local task ids without completion,
dependency, or exception cross-talk.

Run destruction is non-throwing and occurs only after one terminal outcome,
quiescence, release of every lease, and exact release of every reservation and
grant. Dropping a caller observer does not implicitly cancel admitted work.

### Current `GraphRuntime` through Issue #76

The current `GraphRuntime` through Issue #76 owns `GraphModel`, graph-scoped
runtime state, separate graph-state and compute-request lanes, monotonic
`GraphRevision`, revision capture, serialized commit validation/publication,
graph events, stable graph-instance identity, platform/session metadata, and
one `GraphLifetimeAnchor` for that exact Graph identity. The anchor supplies
the close coordinator and lease root used by lifecycle admission.

It owns no Run, admitted-Run index, CPU/device/I/O/plugin worker, process ready
store, process admission, `ResourceLedger`, `PolicyRegistry`, policy binding,
or physical execution route. The process-owned `ExecutionService` owns the
private `RunLifecycleRegistry`; that registry owns the admitted-Run index and
the admission/Graph-close/process-shutdown lifetime fence. `GraphRuntime`
stores only copied HP and RT route ids and their nonzero generations. A Run may
hold a registry-validated Graph lifetime lease without reversing either
ownership.

The graph-state lane is held for immutable revision capture and validated
visible commit, not for long-running planning/execution. The private
compute-request lane currently serializes same-Graph requests without owning
an executor or policy lifetime. The Issue #76 registry/lifetime fence is
current behavior; future work here is limited to public cancellation/control
surfaces and separately approved later execution-domain capabilities, not the
Graph anchor or lifecycle registry.

## Process Execution Domain

`ExecutionService` is a deep module: callers submit ready work and receive
completion; admission, queueing, policy validation, reservations, executors,
and completion routing remain internal.

The product composition root constructs one explicit service from process
configuration and injects it. It is not a static singleton. The root constructs
it before participating Kernels/Hosts and retains it until they stop Run
admission and drain their Runs. Graph close does not stop the service; only
process execution-domain shutdown does.

The current baseline through Issue #75 realizes the shared CPU/resource,
policy, and staged-commit boundary:
`EmbeddedHostState` creates one fixed-pool CPU service with explicit limits
before Kernel, and Kernel injects it into request-local `ComputeService`
instances. Built-in CPU full HP, full RT, standalone dirty HP/RT, and preflight
dispatch transfer immutable, lease-backed `ReadyTaskSubmission` values with
owned callback context. The service executes multiple Runs concurrently and
keeps each Run's completion, first failure, trace routing, and Host context
isolated. Closed private routes provide serial-debug, shared-CPU, and
GPU-pipeline execution without exposing their workers, queues, or completion
adapters. The service exclusively
owns one Host-authoritative ledger, atomically admits each complete Run vector
before publication, requires initial and dependent ready work to enter the same
bounded store with child grants, and releases every reservation/grant exactly
once. The private interactive and throughput strategies apply explicit QoS,
work/byte charging, Graph/Run fairness, deadline preference, aging, interactive
headroom, and bounded throughput progress. Exact Graph identity/revision
validation and staged product publication are current. Private cooperative Run
cancellation closes matching ready admission, purges matching queued entries,
rejects dependent re-entry, waits for in-flight callbacks, and arbitrates with
commit. Per-Graph supersession now coalesces one pending owner per exact key and
requires the current checked generation at commit. Host-authored policy
frontiers, pure-C plugin selection, generation-local sticky fallback,
allocation-free start commit, and ready-to-execution grant exchange are also
current. Issue #76 adds the current lifecycle registry, Graph lifetime leases,
monotonic Graph close, explicit process shutdown, and source-private telemetry;
public cancellation control remains future work.

The service owns physical CPU workers and later resource executors,
bounded ready storage, Run/resource admission, policy-result validation,
execution exception fences, and completion routing. It does not own planning,
dependency semantics, Graph/document persistence, cache authority, dirty
propagation, visible commit, or Graph state.

Its private `RunLifecycleRegistry` owns the one process admission fence, service
accepting/stopping state, graph-indexed open/closing rows, pending admission
candidates, graph-indexed admitted `RunLease` entries, and process-wide Run
enumeration. It is neither Graph-owned, Host-adapter-local, nor static.
Admission first records a pending candidate and obtains a graph-lifetime lease
under this fence, then captures the immutable revision, plans, and obtains a
complete resource reservation. A second fenced recheck atomically installs the
Run in both indexes and is the successful admission linearization point.

Graph close and process shutdown change their lifecycle state through the same
fence. Registration-before-close is indexed and drained; when close wins before
registration, the candidate is rejected and exact rollback completes. Registry
entries hold only a `RunLease` and identity metadata, never the plan,
dispatcher, terminal arbiter, staged output, Graph state, or resource tokens.
They unregister only after terminal publication, physical quiescence,
commit/discard finalization, and exact graph/resource release.

Visible commit enters the graph-state lane before taking the lifecycle fence
for final open-row/registered-Run validation and publication. Close marks
closing and releases the fence before waiting for the lane, so commit-first
publication may finish and close-first validation denies commit without a
registry/lane lock cycle.

`ExecutionService` exclusively owns a Host/device-authoritative `ResourceLedger`
initialized from composition-root limits. Only trusted host code mints its
move-only, non-forgeable reservations and grants. A policy or plugin may request
or suggest resources but cannot construct, duplicate, enlarge, or directly
release a token.

The current ledger validates transactional vectors for CPU slots, ready-store
entries and bytes, retained/in-flight Host memory, and Host scratch. It also
owns isolated immutable memory/scratch limits for each configured non-CPU
`DeviceId`. Native allocation plans commit both dimensions atomically, return
unused bytes after `allocatedSize` reconciliation, and split actual ownership
between persistent native Value owners and asynchronous completion scratch.
Device queue depth/in-flight command limits, compute-I/O operations/bytes, and
plugin-process/invocation/IPC remain future dimensions and are not represented
by fake zero-valued authority. Current success, failure, rejection, rollback,
replacement, worker-exception, stale completion, eviction, cancellation, and
close/shutdown paths release every authority exactly once. Capacity exhaustion
and checked overflow fail without partial reservation, overcommit, cross-device
borrowing, or silent clamping.

Each policy binding is a comparison seam, not a physical executor or resource
authority. The current Interactive and Throughput bindings rank immutable
Host-authored candidate descriptors; the service-owned store retains every
physical entry and Graph/Run fairness row. A policy owns no worker, ready
store, Run, Graph state, budget, reservation, grant/token, native device
handle, executor, completion route, or lifecycle authority.

Issue #71 proves the seam with two real built-in policies and one shared route:

- dispatch cost is `work_units + ceil(complete_ready_grant_bytes / 4096)`;
- Graph service uses raw cost in one accumulator per selected class, and Run
  service uses `ceil(cost / weight)` within each Run's immutable class;
- interactive ordering prefers an earlier present monotonic deadline, while
  throughput ordering is weighted and deterministic;
- a ready entry ages after eight successful dispatches;
- at most three consecutive interactive dispatches precede required
  throughput progress while throughput remains ready;
- configured interactive headroom caps only active Throughput root
  reservations; Interactive Runs do not debit that class quota, while the
  ledger retains final physical authority; the Throughput charge follows exact
  root lifetime through deferred child release; and
- initial and dependent work use the same policy route, retaining Run rows
  across temporary emptiness.

Latest-generation preference and exact-key coalescing are current from #74.
Revision-safe commit is current from #72, cooperative cancellation is current
from #73, and pure-C policy ABI v1, generation-scoped bindings, Host-authored
frontiers, validated fallback, reserved start, and private execution routes are
current from #75.
Larger quanta and device-utilization awareness remain later profile/device
targets; issue #71 does not claim them.

The worker-owning scheduler ABI, SDK target, `IScheduler` hierarchy, and
per-Graph physical owners have been removed completely. The pure-C policy ABI
is the breaking replacement; no compatibility adapter or forwarding layer
remains.

The former worker-only budget has been removed completely rather than wrapped,
renamed, or aliased. Execution worker-count resolution is composition-root
configuration only; all CPU admission authority for Runs comes from the one
service-owned ledger.

### Revision, cancellation, and visible commit

Issue #72 makes the minimum revision subset current. A Run captures one strong
Graph instance identity and immutable `GraphRevision` before planning. Product
work uses request-owned Graph/proxy snapshots. Its serialized predicate requires
`CommitPending`, the expected domain/label, the exact staged owners, staged
identity/revision equality with the descriptor, live identity/revision equality,
and valid staged domain output. Successful publication preserves the revision
and precedes Run success. Failed validation discards staged output and cannot
mutate visible Graph/proxy state or write deferred cache artifacts.

Issue #73 makes cancellation part of that current predicate. One private
request source and immutable monotonic deadline contend through the same Run
arbiter as Run-internal failure and the Graph/RT result-commit contender.
Built-in ready entries are purged by exact Run identity, dependent re-entry is
rejected, queued plan/callback completion units retire exactly once, and
entered non-preemptible work drains without permitting staged publication. The
accepted commit contender, exact predicate, eligible persistence, visible
swap, and terminal resolution share one serialized graph-state work item.

Issue #74 extends that predicate with a current supersession generation.
Supersession selects a newer generation and requests
cancellation of older matching Runs without reusing their identity or mutating
their plans. Non-preemptible work and external side effects may finish, but
stale, cancelled, failed, or overdue output cannot commit.

Issue #76 further adds the current `Open` registry Graph row, registered Run,
and valid Graph lifetime lease checks.

Any future compatible-revision optimization requires another explicit decision;
compatibility is not inferred from equal topology.

Paired RT/HP work uses a monotonic `Pending` / `RtCommitted` / `Denied` sibling
gate. Issue #72 currently opens it only after valid RT proxy publication and
then applies an independent HP revision predicate; a later stale HP result does
not roll back RT. Issue #73 makes RT cancellation while `Pending` deny the gate
and request HP cancellation; HP cancellation after `RtCommitted` cannot roll
back RT. Graph-close and process-shutdown denial reasons now fan through both
child Runs.

### Close and shutdown scopes

Graph close marks its row closing under the lifecycle fence, rejects
new/pending admission, waits prior candidates to register or roll back, and
enumerates the complete graph Run index. It denies visible commit, cancels or
drains those Runs, and preserves their finalization paths. Only after terminal
publication, physical quiescence, commit/discard finalization, exact graph/
resource release, and admitted-Run unregistration does it remove the empty row,
stop/drain the compute-request lane while graph-state finalization remains
available, stop/drain the graph-state lane, and destroy Graph state. Unrelated
Graph Runs and the shared service continue; marker completion never reopens
either lane.

Process execution-domain shutdown marks the service stopping and all graph rows
closing under the same fence, resolves pending candidates, and enumerates the
complete process Run index. Bounded ready submission, execution, completion
routing, and graph-state finalization remain available only for already-admitted
Runs chosen to cancel or drain. After every Run settles, releases graph/resource
leases exactly once, and unregisters, shutdown stops remaining work admission,
joins all physical executors, retires policy bindings, publishes
`ServiceStopped` with all 15 lifecycle/resource counters zero, and destroys the
service.
Worker/operation exceptions are fenced and routed through the matching Run
lease; late completion performs cleanup only.

### Delivery dependency contract

| Issue | Required outcome | Depends on |
| --- | --- | --- |
| [#66](https://github.com/kevin-zf1123/photospider/issues/66) | Current HP `ComputeRun` descriptor, state, storage, and one terminal outcome | #63, #65 |
| [#67](https://github.com/kevin-zf1123/photospider/issues/67) | Current stable Run leases and `(RunId, RunLocalTaskId)` full-HP completion isolation | #66 |
| [#68](https://github.com/kevin-zf1123/photospider/issues/68) | Injected CPU-only service foundation, one Run, ready-only input | #67 |
| [#69](https://github.com/kevin-zf1123/photospider/issues/69) | Shared multi-Graph/HP/RT CPU domain and no per-Graph CPU workers | #68 |
| [#70](https://github.com/kevin-zf1123/photospider/issues/70) | Current production admission, bounded ready store, and ledger | #69 |
| [#71](https://github.com/kevin-zf1123/photospider/issues/71) | Current interactive and throughput built-in policies | #70 |
| [#72](https://github.com/kevin-zf1123/photospider/issues/72) | Current revision capture and staged commit predicate | #67 |
| [#73](https://github.com/kevin-zf1123/photospider/issues/73) | Current queued/running/commit cancellation | #70, #72 |
| [#74](https://github.com/kevin-zf1123/photospider/issues/74) | Current latest-wins supersession and realtime `RunGroup` | #71, #73 |
| [#75](https://github.com/kevin-zf1123/photospider/issues/75) | Current pure-C policy generation, Host frontier/fallback, reserved start, and private execution routes | #71 |
| [#76](https://github.com/kevin-zf1123/photospider/issues/76) | Graph close, process shutdown, telemetry, final invariants | #69, #73, #74, #75 |

The graph is acyclic. #72 was permitted after #67 in parallel with #68–#71;
#75 was delivered after #71 alongside the #73–#74 line. The table freezes
ownership dependencies, not implementation algorithms.

## Dependency-Neutral Kernel

[ADR 0002](../adr/0002-external-libraries-are-kernel-adapters.md)
governs this target. The maintained current baseline is documented in
[Kernel Terminology](../kernel-architecture/Terminology.md),
[Kernel Data Model](../kernel-architecture/Data-Model.md),
[Dirty Region Propagation and Work Selection](../kernel-architecture/Dirty-Region-Propagation.md),
and [Graph Lifecycle and Mutation Semantics](../kernel-architecture/Graph-Lifecycle.md).
Those current-state documents remain authoritative while the migration
proceeds.

The kernel owns only the small primitives needed to express and execute its
semantics:

- checked rectangles, extents, clipping, union/intersection, scale, halo, grid,
  tile alignment, and transform bounds;
- stride-aware buffer view, copy, fill, crop-to-view, pad, minimal conversion,
  and validation primitives;
- format-neutral parameter values and typed graph definitions;
- injected graph document readers/writers, image/artifact codecs, and cache
  metadata codecs.

OpenCV remains valuable as an optional operation provider, image codec, and
public image adapter. It must not define Graph, ROI, dirty propagation,
planning, cache, or runtime interfaces. The current repository-owned CPU
provider already follows the provider concurrency direction from
[ADR 0004](../adr/0004-opencv-cpu-operations-are-reentrant-provider-work.md):
it uses reentrant `cv::Mat` callbacks, fixes OpenCV internal CPU threading at
one before publication, leaves outer parallelism to Host-admitted execution
starts, and keeps genuine shared backend synchronization provider-local. The
repository-owned operation algorithms, their OpenCV initialization, and their
exception translation now live in a separately switchable provider module;
the provider-disabled profile proves a stdlib-only v2 provider can supply and
execute an absent operation. Issue #63 makes image processing, codecs, public
adapters, provider/plugin defaults, and the embedded product capability
selected. The dependency-disabled profile discovers no OpenCV and builds the
real kernel aggregate and Host product with standard-library or explicit
unavailable adapters.

YAML remains a supported document adapter. `YAML::Node` must not remain the
runtime parameter, output, cache metadata, or graph-state value model. Graph
loading and saving are injected behaviors with explicit transaction and error
contracts. [ADR 0005](../adr/0005-graph-document-ingestion-is-a-classified-transaction.md)
fixes the classified ingestion transaction that the loading boundary must
preserve.

Issue #62 makes the runtime/cache value slice current: shared YAML conversion
is adapter-owned, cache metadata crosses an injected format-neutral codec, and
inspection uses a neutral recursive formatter. Issue #63 completes the
dependency-disabled product/static/install consumer slice. Its clean smoke
build disables both capability discoveries, verifies the real
`photospider_kernel` and `photospider` targets, installs without dependency
leakage, and runs an external Host consumer.

## General Data and Regions

Current baseline: `ImageBuffer`, `DataType`, `Device`, `PixelRect`,
`ParameterMap`, operation ABI v2, and the existing cache/execution ownership
remain implemented compatibility contracts. V-2 implemented a bounded
dependency-neutral CPU DenseTensor `Value`/`ImageView` subset and one built-in
operation. V-3 now adds checked BufferHandle ownership, lease-controlled
construction, process-local allocation/revision identity, bounded signed
layouts, and formal HP cache identity authority for CPU image Values. V-4 now
adds the public bounded Region contract, logical dirty/cache validity, and
ImageRect/TensorSlice execution through the exact core dense path. V-5 routes
CPU implementation metadata and checked resource demand.
V-6 now adds a dependency-neutral ReadyFence/Value readiness contract and one
explicit source-private CPU Value-copy task proved with a deterministic fake
device executor. V-7 now adds a fixed source-private
`DeviceExecutorRegistry` to the process execution domain and runs the
repository Metal Perlin operation through its owned device/queue,
invocation-scoped allocator, and persistent pipeline cache. V-8 now adds
checked CPU/Metal binding facts, pure explicit access planning,
revision-preserving bidirectional transfer, process-owned residency, exact
stale-completion arbitration, pending-Value continuation, and asynchronous
Perlin readback. V-9 now adds isolated memory/scratch accounts only for
executable devices in the fixed registry, admits native plans before
allocation, reconciles allocator-reported actual bytes, and binds exact leases
to persistent Values and asynchronous completion. V-10 ratifies typed
compute-I/O completion and keeps persistence authorities separate; V-11 runs
the first bounded cache/codec mechanism through `ComputeIoExecutor`. V-12 now
verifies the installed generic model across 1/3/4/8/16-channel FP32/FP64
images, rank-one through rank-five FP32/FP64 latent Values, padded and
signed/zero strides, exact Region merge, explicit CPU/external-device transfer,
and bounded compute-I/O retention. V-13 now installs one packed FP4 E2M1,
block-scale quantized DenseTensor vertical with version-1 Blocked addressing,
checked packed access, block-aligned TensorSlice copy, representation-preserving
transfer, exact memory-cache retention, and fail-closed image disk persistence.
V-14 now installs a dependency-neutral provider-defined Value vertical with
byte-preserving Schema/Facet/Layout envelopes, checked multi-buffer bindings,
one injected typed registry, pure-C definition-suite ABI v3, pure
property/DataSpec/Region evaluation, canonical descriptor/content/layout
digests, artifact-envelope round-trip, and generation-safe replacement/unload.
V-15 now binds that unchanged generic model to an optional repository OpenEXR
single-part deep-scanline provider/codec, with explicit channel identities,
typed shape/error rejection, bounded compute-I/O execution, generation-safe
lifetime, and a dependency-clean default-OFF package profile. Their exact
behavior is documented in
[Kernel Data Model](../kernel-architecture/Data-Model.md),
[ImageBuffer Memory Contract](../kernel-architecture/ImageBuffer-Memory-Contract.md),
[Plugin ABI](../kernel-architecture/Plugin-ABI.md), and
[Kernel Cache Model](../kernel-architecture/Cache-Model.md), with execution
ownership in
[Policy and Execution Architecture](../kernel-architecture/Policy-and-Execution-Architecture.md)
and [Compute Boundaries](../kernel-architecture/Compute-Boundaries.md). The
complete model below is the accepted target; only the explicit V-2 through
V-15 subset called out here is a current runtime fact.

[ADR 0008](../adr/0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md)
is authoritative for the complete target contract. Its central separation is:

```text
Value
├── DataDescriptor
│   ├── exactly one versioned RepresentationSchema
│   └── zero or more orthogonal versioned Facets
└── one or more authoritative StorageBindings
    ├── StorageLayout
    ├── BufferHandle[]
    ├── ReadyFence
    └── AccessProvider lease
```

`DataDescriptor` is logical. Allocation, stride, packing, device, byte range,
mapping, and readiness are physical binding facts. `Value` is logically and
structurally immutable after one exclusive `ValueBuilder::seal`; checked views
retain the complete Value. Seal revokes every ordinary builder/caller
`WriteLease` and every consumer write path. A Pending producer may retain only
the unique private write capability transferred atomically at seal for its
prevalidated binding envelope. The stable core is extensible through permanent
Schema/Facet/Layout identities, canonical versioned payloads, pure nonblocking
queries, explicit operations, and immutable process-owned provider generations
with leases.

The first representation is homogeneous rank-N DenseTensor. An ordinary image
is `DenseTensor + ImageFacet`; channel, color, alpha, and time meaning is
explicit and never inferred from names. Per-site variable samples use
`VariableSampleField`; an OpenEXR Deep logical value is
`VariableSampleField + ImageFacet + DeepSampleFacet`. StructuredValue v1 is
self-contained and does not contain runtime child Values.

The implemented V-2 through V-15 subset is deliberately narrower:

- `DenseTensorDescriptor` contains positive concrete shape, independent
  unsigned/signed integer or floating element semantics, 8/16/32/64-bit native
  scalar storage or the explicit four-bit FP4 E2M1 encoding, and optional V-13
  block-scale quantization with a rank-matched positive block shape and one
  finite positive scale per complete row-major logical block;
- `ImageFacet` explicitly maps distinct x/y and optional channel axes;
- public `BufferHandle` is a checked nonempty range over one opaque
  process-local `AllocationIdentity`; subranges retain allocation lifetime.
  CPU allocations may issue host read leases, while source-private native
  bindings retain an external owner and expose only checked binding facts;
  neither path exposes a public raw or native pointer;
- move-only `ValueBuilder` controls the only move-only `WriteLease`, requires a
  zero-offset positive exact-envelope Strided producer or a version-1
  nibble-aligned exact-envelope non-overlapping Blocked producer, refuses seal
  while the lease is live, and publishes a fresh process-local
  `ValueRevisionId`;
- final copyable `Value` shares immutable descriptor/layout/handle state;
  DenseTensor Values over sealed handles retain exactly one tagged Strided or Blocked
  layout; Strided aliases may use a bounded byte offset and positive, zero, or
  negative signed strides, while V-13 Blocked aliases use checked bit offsets
  and block bit strides;
- retaining checked `DenseTensorView`/`ImageView` hold a `ReadLease` and expose
  read-only whole-byte addresses; `PackedDenseTensorView` instead exposes
  checked FP4 codes and scale-dequantized values without a fake element byte
  pointer;
- installed `ReadyFence` is a copyable nonblocking observer of Pending, Ready,
  Failed, or ProducerCancelled; its move-only completer publishes one terminal
  state, dropped completion publishes cancellation, and waits are enqueued
  through a shared non-inline executor retained while pending or queued and
  through callback completion, with queued self-retention released on callback
  entry;
- synchronous Values start Ready, while source-private CPU and native pending
  producers retain the only mutable completion capability and revoke it before
  every terminal state; pending/failed/cancelled Values preserve immutable
  metadata but reject BufferHandle and checked-view payload access;
- checked `DeviceId`, `MemoryDomain`, `StorageBinding`, producer identity, and
  pure `AccessPlan` make direct, map, import, transfer, or unsupported access
  explicit. `ValueTransferTask` preserves the logical revision across a fresh
  destination binding and performs CPU copy, CPU-to-Metal upload, or
  Metal-to-CPU readback only as explicit queued work after source readiness;
- one process-owned `ResidencyManager` indexes eligible exact revision/binding
  replicas. It atomically validates the complete Graph/Run/generation/task/
  producer/binding completion identity, publishes readiness, and inserts
  residency, so late, duplicate, or mismatched completions cannot release
  dependency work or regain a stale commit right. Kernel pretracks each
  lineage before coordinator submission, and only accepted current publication
  advances that row before currentness becomes observable, preventing a later
  older Run start from regressing freshness;
- source-private `DeviceExecutorRegistry` composition owns fixed non-CPU
  executors under `ExecutionService`; in the enabled Apple repository-plugin
  profile, the Metal executor owns one reusable native device/queue and
  validated pipeline cache, retains
  callback-scoped textures/buffers through an invocation allocator, and enters
  one selected Perlin operation after reserved start without exposing native
  handles through Graph, policy, metadata, or public Host state. Perlin
  publishes a pending native Value and encodes asynchronous texture-to-shared-
  buffer readback without command-buffer waits or synchronous `getBytes`;
- service composition validates all candidate per-`DeviceId` limits, then
  creates device memory/scratch accounts only for matching executors in the
  frozen registry. Empty or non-Apple default registries expose no Metal
  account, while a registered executor without a candidate budget remains
  unable to admit native allocation;
- `image_process:invert_dense` separates exact descriptor-only inference from
  stride-aware unsigned-8 execution, reuses a sealed input Value when present,
  and publishes the exact sealed result revision plus an independent
  ImageBuffer compatibility snapshot; and
- private formal HP CPU image cache entries treat a valid sealed
  `NodeOutput::image_value` as allocation/revision authority. Ordinary copies
  preserve identity; dirty mutation, replacement, and disk decode create fresh
  identity; disk save reads Value bytes; and runtime tokens are never
  persistent cache/task keys. V-13 formal memory-cache copies also retain
  packed Values and exact TensorSlice validity, while the image-only disk cache
  rejects packed, quantized, or latent formal Values before executor admission,
  filesystem mutation, or codec calls;
- installed `RegionSet` supports canonical Empty/Whole, one bounded nonempty
  conjunction of ImageRect or rank-general TensorSlice atoms, checked
  normalization/clipping/algebra/containment, explicit budgets, and typed
  Exact/ConservativeSuperset/Unknown/Unsupported/TooComplex outcomes;
- dirty source, per-node, edge, monolithic, and HP validity records retain
  normalized Region, while current image tiles, ImageBuffer helpers, Host/IPC
  v2 inspection, and operation ABI v2 use checked derived PixelRect; and
- the exact selected core `invert_dense` callback executes ImageRect or
  TensorSlice through checked strides; TensorSlice is HP-only monolithic work,
  and same-key plugin replacement cannot inherit that source-private contract.

V-14 adds a second explicit `ProviderDefined` representation. Its
`DataDescriptorEnvelope` owns one Schema and bounded ordered Facets; its
`ProviderDefinedLayout` owns one Layout definition plus checked buffer-role
envelopes; and its Value retains multiple sealed host-readable `BufferHandle`s
plus one immutable provider generation. DenseTensor-only accessors and current
transfer paths reject this representation. Indexed `ProviderReadLease` retains
both the selected buffer and interpretation generation.

One injected `DataDefinitionRegistry` owns a single generation source,
provider table, and strict typed Schema/Facet/Layout maps under one publication
lock. It stages and validates complete candidate bundles, publishes new or
replacement generations atomically, rejects cross-provider typed-key
conflicts without partial visibility, and invokes no provider callback while
locked. Unload denies new lookup while old Values, reads, callbacks, and opaque
owners retain the retiring generation through final provider destroy and
module release.

V-12 adds verification rather than a new representation or provider ABI. Its
dependency-neutral matrix proves active logical FP32/FP64 image elements for
1/3/4/8/16 channels through padded Values and the CPU ImageBuffer bridge;
rank-one through rank-five FP32/FP64 latent Values through full-rank
TensorSlice; selected/unselected ImageRect/TensorSlice merge; complete positive
producer-envelope preservation across explicit CPU and injected external-device
transfer; exact binding, allocation, revision, and Pending-to-Ready facts; and
immutable negative/zero-stride reads plus explicit transfer rejection. An
independent direct-offset byte oracle proves that the rank-one sole stride is
wider than the element, its required storage span is padded, its active bytes
remain exact, and its padding sentinel is untouched. CPU-copy and external
preparation reuse one core positive, zero-offset, exact-envelope, non-overlap
authority; external rejection precedes destination-owner retention, identity
minting, fence creation, provider invocation, and Pending publication without
narrowing the general signed immutable publisher. An admitted compute-I/O task
retains and observes the same Value metadata and bytes under bounded budgets,
but creates no artifact or persistence identity.

V-13 adds one executable packed vertical. FP4 E2M1 encoding, floating-point
semantics, and block-scale quantization remain independent facts. Version-1
`BlockedLayout` records matching block shape, nibble-aligned block bit strides,
absolute bit offset, and explicit nibble order. Checked publication proves
complete blocks, exact byte bounds, and non-overlapping block spans. The packed
TensorSlice copy accepts only full-rank nonempty block-aligned intervals,
projects row-major scales, directly copies codes, and publishes a fresh
canonical blocked CPU Value. CPU and injected external-device transfer preserve
descriptor, quantization, layout, byte envelope (including unused nibble bits),
logical revision, and Pending-to-Ready facts in a distinct binding. Formal
memory cache retains the exact Value/Region facts; image disk persistence fails
closed without inventing widened image bytes or a generic artifact format.

V-14 implements one bounded concrete `DataSpec`, typed pure property and Region
outcomes, the exact-size C11/C++17 v3 definition-suite ABI, and tagged SHA-256
Descriptor/Content/StorageLayout digests. Pure callbacks receive no payload;
validation and canonical-content traversal receive only retained checked
buffer views. Versioned artifact-envelope encoding preserves unknown
Schema/Facet/Layout bytes and digest metadata without a provider. It is not a
graph document, filesystem codec, cache manifest/chunk store, or durable output
authority.

V-15 implements the first concrete optional `VariableSampleField` +
`ImageFacet` + `DeepSampleFacet` codec. Its v3 provider publishes four fixed
definitions and uses explicit versioned mapping metadata; diagnostic channel
names never imply roles. A canonical provider-defined Value contains row-major
counts, checked prefix offsets, and one identity-ordered FP32 stream per
unit-sampled channel. Reusing V-14's nonempty semantic-buffer invariant bounds
an all-zero image to count/offset storage only; channel mappings remain in
versioned metadata without a sentinel payload or zero-length envelope.
The source-private adapter reads and writes complete single-part deep-scanline
files, materializes through the injected registry, retains exact generation
and Value/read leases, and translates every foreign failure to Host-owned
errors. Each indivisible codec call runs as one positively budgeted
`ComputeIoExecutor` task with OpenEXR internal threads disabled.

V-15 still has no public device registry, device queue/in-flight dimensions,
additional packed encodings or quantization formulae, unaligned requantizing
slices, access/conversion/inference/execution provider suites, generic graph or
cache Value persistence, manifests/chunks, deep-tiled/multipart/mixed-part
OpenEXR, or general named graph Value outputs. Its native executor, transfer
submission, mutable producer, completion admission, and residency owner remain
source-private.
ImageBuffer remains the
compatibility representation for operation ABI v2, tiled writes, existing
image codecs, and Host surfaces; V-15 does not adapt its deep Value through it.

`ElementSemantics`, `StorageEncoding`, and `QuantizationSchema` are independent.
Describable, executable, and convertible support are also independent, and
conversion is always explicit. This allows FP64, arbitrary channels, padded or
signed strides, N-dimensional latent values, and packed FP4 to be represented
without silent float32 conversion, one-byte-per-element assumptions, or
channel-role guessing.

For the current V-15 subset, `BufferHandle` is a checked immutable byte range.
Consumer reads and ordinary builder writes require leases; sealed Values never
issue `WriteLease`, and consumer writes are always rejected. A source-private
producer may complete one sealed pending CPU or native payload through its
noncopyable capability inside the prevalidated binding/Layout/handle envelope.
It retires that capability happens-before publishing Ready, Failed, or
ProducerCancelled. Pending, Failed, and ProducerCancelled expose no
consumer-readable payload. A CPU binding may provide direct host visibility;
a device-local binding does not. Strided, Blocked, and ProviderDefined Layouts
retain bounded buffer envelopes. `DeviceBackend`, `DeviceId`, and
`MemoryDomain` are separate, and current access is explicitly represented by a
`Direct | Map | Import | Transfer | Unsupported` plan. Only direct CPU access
and explicit CPU/Metal transfer have production execution in V-8; the other
plan kinds remain typed outcomes rather than hidden work.

The implemented `RegionSet` is bounded DNF over explicit logical domain keys.
The MVP supports Whole, Empty, ImageRect, TensorSlice, and one nonempty clause.
Region algebra returns Exact, labelled ConservativeSuperset, Unknown,
Unsupported, or TooComplex rather than silently widening. The V-14 provider
subset of `DataSpec` constrains Schema identity/version and logical-site bounds
and returns subset, disjointness, a conditional runtime guard, or
`CannotEvaluate`; it never authorizes implicit conversion or device access.

Runtime revision, descriptor/content/Layout digests, and artifact identity are
different identities. Persistence is divided into graph documents, canonical
descriptor envelopes, artifact/cache manifests plus chunks, and
never-persisted runtime bindings. Unknown valid extension bytes are preserved
without interpretation when a provider is absent.

The public migration is complete rather than permanently dual:

```text
ImageBuffer     -> Value + ImageFacet + ImageView
PixelRect       -> RegionSet atom ImageRect
Device          -> DeviceBackend + DeviceId + MemoryDomain
OperationOutput -> named Value outputs
ParameterMap    -> configuration only
```

Operation providers migrate from provisional C++ ABI v2 to separately
versioned pure-C provider ABI v3 only after exact records and owned consumers
exist. The completion boundary deletes v2 without a permanent wrapper, alias,
forwarding header, dual loader, or v2-to-v3 shim. Policy ABI v1 remains
independent.

### Project 4 implementation dependency contract

The table fixes architectural ordering, not live completion status. Each linked
Issue remains a separately verifiable implementation slice and its Issue and
Project fields remain the status authority.

| Slice | Delivery boundary | Blocking slices |
| --- | --- | --- |
| [#78 / V-1](https://github.com/kevin-zf1123/photospider/issues/78) | Ratify the generic data, memory, and Region ADR; documentation only | #63, #65 |
| [#79 / V-2](https://github.com/kevin-zf1123/photospider/issues/79) | Run one operation with CPU DenseTensor plus ImageView | #78 |
| [#80 / V-3](https://github.com/kevin-zf1123/photospider/issues/80) | Connect BufferHandle ownership, allocation identity, and cache | #79 |
| [#81 / V-4](https://github.com/kevin-zf1123/photospider/issues/81) | Run ImageRect and TensorSlice through unified Region | #79, #72 |
| [#82 / V-5](https://github.com/kevin-zf1123/photospider/issues/82) | Drive CPU implementation and resource routing from operation metadata | #80, #70 |
| [#83 / V-6](https://github.com/kevin-zf1123/photospider/issues/83) | Prove fences, asynchronous completion, and explicit transfer with a fake device | #80, #81, #82, #70 |
| [#84 / V-7](https://github.com/kevin-zf1123/photospider/issues/84) | Run one Metal operation through DeviceExecutorRegistry | #83 |
| [#85 / V-8](https://github.com/kevin-zf1123/photospider/issues/85) | Implement explicit CPU/GPU transfer, residency, and stale completion | #84, #74 |
| [#86 / V-9](https://github.com/kevin-zf1123/photospider/issues/86) | Account device memory and scratch in ResourceLedger | #84, #70 |
| [#87 / V-10](https://github.com/kevin-zf1123/photospider/issues/87) | Ratify typed compute-I/O completion and separate cache, Graph-document, daemon, and durable-output authorities; documentation only | #65 |
| [#88 / V-11](https://github.com/kevin-zf1123/photospider/issues/88) | Route bounded cache/asset/codec I/O mechanism through `ComputeIoExecutor` without moving commit policy | #87, #70 |
| [#89 / V-12](https://github.com/kevin-zf1123/photospider/issues/89) | Verify the multi-channel, FP64, latent, and stride matrix | #81, #85 |
| [#90 / V-13](https://github.com/kevin-zf1123/photospider/issues/90) | Run one packed FP4/quantized DenseTensor slice | #89 |
| [#117 / V-14](https://github.com/kevin-zf1123/photospider/issues/117) | Prove dependency-free VariableSampleField definitions, multi-buffer Values, pure queries/digests, and generation replacement/unload | #90 |
| [#118 / V-15](https://github.com/kevin-zf1123/photospider/issues/118) | Add the first optional OpenEXR deep-scanline provider/codec without leaking the dependency | #117 |

V-14 is the current separate dependency-free synthetic
`VariableSampleField` slice. “Dependency-free” means the proof uses neither
OpenEXR nor another optional codec. It exercises registration, unknown-byte
preservation, multi-buffer Layout and binding, Region/DataSpec/query without
payload authority, independent exact canonical digests, generation
replacement, leases, and unload directly. Its ABI v3 is the definition suite
only and does not pre-implement access, conversion, inference, execution, or
codec authority.

V-15 is the current separate optional OpenEXR provider/codec slice. Its first
format is single-part deep-scanline read/write, following the core and V-14
proof rather than replacing it. Deep tiled, multipart, and mixed shallow/deep
parts remain later work. The build option defaults OFF; that profile removes
OpenEXR headers, links, types, symbols, package discovery, target exports, and
transitive dependencies from the kernel, public ABI, and dependency-disabled
product. Explicit component consumption is the only installed package path
that discovers OpenEXR and imports the provider MODULE.

## Heterogeneous Executors

A current V-9 Metal route combines process ownership, registry dispatch,
queue/allocator/cache reuse, provider-state removal, asynchronous pending
Values, explicit CPU/Metal transfer, process residency, and exact stale-result
arbitration. Its sole service ledger now atomically admits per-device
memory/scratch plans before native allocation, reconciles native actual bytes,
binds memory to persistent Value ownership, and binds scratch to exact command
completion. Queue, lane, and pipeline-cache infrastructure remain outside
per-invocation accounting.

A GPU executor is not a second ordinary CPU worker pool. Each physical device
executor owns its native queue/stream, allocator, in-flight limit, memory and
scratch reservations, pipeline cache, transfer queues, and completion fences.
CPU workers do not block waiting for GPU completion. A stale device completion
releases resources but cannot commit to a newer graph revision.

Current V-11 adds one source-private process `ComputeIoExecutor` with an
independent worker and atomic task/estimated-retained-byte admission before
lazy payload construction or side effects. Accepted work retains an explicit
transaction lifetime token and exposes typed completion with exactly-once
settlement across failure, cancellation, late return, and shutdown. CPU
compute workers cannot synchronously wait for it.

The first production vertical runs staged HP cache-save codec/filesystem
mechanism through this executor while graph-state policy keeps eligibility,
paths, error interpretation, and the existing pre-publication commit point.
The current indivisible image-codec call runs wholly on the I/O worker; a
future split API must return independently admitted CPU-heavy phases to the
CPU executor. Synchronous cache administration/load, daemon framing,
Graph-document persistence, `OutputStore` commit policy, user paths, retries,
and durability claims remain with their existing owners.

### Compute I/O durability and completion target

The current baseline has the bounded executor and staged HP cache-save vertical
described above, but no crash-durable output store. Deferred HP cache writes
still occur before live Graph publication and can fail the Run; Graph-document
save writes its destination directly; daemon job state and acknowledgement are
process-local; and the private IPC `OutputStore` provides protected,
no-replace process-scoped delivery with in-memory lease/TTL indexing. The
legacy `io/save` callback can also expose a file before its enclosing staged
Run commits.

[ADR 0009](../adr/0009-compute-io-durability-and-completion-semantics.md)
accepts a target typed partial order:

```text
successful value-producing Run:
  OperationReturned(success)
    -> producer fences succeed
    -> ValueReady
    -> validated Graph/RT publication
    -> RunTerminal(Succeeded)

pre-terminal ComputeRun failure:
  operation/readiness/dependency failure
  OR Graph/RT validation/publication/Run-result commit failure
  typed failure -> RunTerminal(Failed)
  (no fabricated ValueReady or OutputCommitted)

Run cancellation:
  cancellation wins -> RunTerminal(Cancelled)
  late/stale completion -> cleanup only
  (no new ValueReady or durable receipt)

validated empty-plan / zero-work / no-op:
  no-work validation -> RunTerminal(Succeeded)
  (no new OperationReturned, ValueReady, or durable receipt)

RunTerminal(Succeeded) -> ResultAvailable   (when a result is retained)

compute-and-persist success =
  RunTerminal(Succeeded) AND OutputCommitted

post-Run output transaction failure =
  RunTerminal(unchanged) AND OutputCommitFailed
  (no new or revoked ValueReady; no requested-durability receipt)

RequestAccepted, OutputCommitFailed, GraphDocumentSaved, and ResponseObserved
are separately ordered by the operation that owns them.
```

Only dependency-valid Graph/RT publication, or an admitted valid no-op,
resolves `ComputeRun::Succeeded`. Cache persistence, durable output commit,
Graph-document save, daemon terminal state, result availability, and caller
observation remain independent outcomes. Post-Run cache, codec, and output work
has its own typed outcome and cannot delay or rewrite the published Run
terminal. An output failure after Run terminal reports `OutputCommitFailed`,
not `RunTerminal(Failed)`, and neither creates nor revokes `ValueReady`. A
caller or daemon may report a composite request failure, but it preserves the
Run terminal and the output, Graph-document, cache/codec, and response facts
instead of projecting that aggregate result back into Run state. A receipt
committed independently before a later Run cancellation remains authoritative
for that output transaction.

| Persistence domain | Target authority | Completion and durability contract |
| --- | --- | --- |
| Graph document | Graph-state save transaction | Versioned, same-directory staging with expected-version validation, atomic replacement, and an explicit achieved-durability result |
| Disk cache | Graph cache policy using bounded I/O mechanism | Discardable acceleration; failure does not rewrite successful Run/output outcomes |
| User output | `OutputStore` commit authority | Stable `OutputCommitId`; complete payload/metadata validation and file synchronization; canonical manifest validation, synchronization, and atomic no-replace publication; leaf-to-durability-root directory barriers; typed achieved-durability receipt or `OutputCommitFailed`; recovery; and no-overwrite by default |
| Daemon transport | Job registry and result delivery | Acceptance, terminal state, and response observation only; no durability inference |
| Codec | Injected representation adapter | Conversion and error translation only; no path, retry, identity, or commit authority |

Durable output retry is idempotent by stable commit identity and delivery is
at least once, not exactly once. Cancellation before manifest publication may
abort and clean staging; after the manifest commit point it reports the
committed receipt rather than pretending the output was cancelled. Requested
crash durability requires complete payload/metadata validation and file
synchronization, a completely written and validated canonical manifest,
manifest-file synchronization before atomic no-replace publication, published
identity validation, and directory barriers for every directory created,
renamed, or modified by the transaction from the leaf to the configured
durability root. An atomic-visible receipt can be returned after its weaker
commit point; a crash-durable receipt is returned only after all stronger
barriers succeed. Unsupported file synchronization, directory barriers, or
atomic no-replace publication fail explicitly; durability is never silently
downgraded.

All persistent paths are rooted and normalized, reject escapes and symlink
substitution through no-follow/identity checks, apply quotas before retained
work, and expose achieved durability as a capability/result. The
`ComputeIoExecutor` supplies bounded mechanism only; the domain authorities
above retain identity, ordering, policy, and receipt ownership.

## Execution Profiles

[ADR 0010](../adr/0010-execution-profile-slos-are-six-independent-benchmark-verdicts.md)
freezes the `execution-profile-slo-v1` target. It defines one exact generated
RGBA FP32 source and four-node `curve_transform` graph family, then four
immutable workload ids:

| Workload | Target role |
| --- | --- |
| `I1-edit-storm-v1` | Natural edit ordinals `1..12` map to `edit_index=0..11`; twelve exact parameter/256x256-Region edits use one latest-wins key, Interactive QoS, a monotonic nominal cadence with bounded start lateness, and twelfth-edit (`edit_index=11`) visibility. |
| `I2-progressive-v1` | The exact I1 Graph/target/revision and edit mapping uses its separate legal realtime request key with RT-preview and HP-final child contracts; the twelfth edit (`edit_index=11`) publishes a 512x512 preview followed by the 2048x2048 final, with exact Host/conditional-Metal residency reuse and zero hidden I/O/copy. |
| `B1-immutable-v1` | Thirty job-indexed immutable full-frame jobs are offered in order across two Graphs, with bounded Compute I/O task/planned-byte admission, canonical raw artifacts/manifests and semantic traces, crash-durable receipts, and logical/raw goldens at Run caps 1 and 8. |
| `M1-shared-v1` | Forty exact I1 starts and continuously offered cap-8 B1 cycles sharing one process execution authority for 30 measured seconds. |

Latency, throughput, fairness, determinism, waste, and memory are six
independent verdicts. Interactive latency has absolute p50/p95/p99 gates;
batch throughput and B1/I2 memory have immutable same-environment reference
gates; mixed load additionally has a 0.20 p05 Throughput-progress floor, a
0.95 p05 two-Graph Jain index, the three-to-one class-start bound, zero
headroom-caused Interactive admission failures, and isolated-relative latency.
Exact output/artifact/semantic-trace/golden digests, bounded discarded service,
absolute resource limits, and exact quiescent settlement cannot be traded for
speed in another dimension.

For B1 and M1, “same environment” uses ADR 0010's closed manifests: a fixed
24-field `execution-profile-base-environment-v1`, fixed 21-field
`execution-profile-storage-environment-v1`, and fixed four-field
`execution-profile-environment-class-v1`. Their ASCII length-framed canonical
bytes and independently recomputed SHA-256 values must match exactly; digest
equality never substitutes for byte equality. The storage schema fixes typed
state/reason pairs, the only allowed N/A reasons, a seven-key effective-mount
map, six commit-semantics keys, durability endpoint/anchor identities, and
closed capability/enum sets. Its fixed 37-component B1 performance record binds
compression, encryption, checksum/deduplication, block/record/allocation units,
provisioning/layout geometry, upper write cache, I/O scheduling/queue/
concurrency, remote network path, backend service tier, and device profile.
Any effective option that can affect the complete measured storage path is
mapped or proved irrelevant; opacity fails closed, while instantaneous load
noise remains raw diagnostic evidence. Remote, RAM-backed, or copy-on-write
storage is capability-gated rather than accepted or forbidden by class. #95
implements the fixed probe-to-schema mapping, single encoder, eligibility, and
B1 checks without changing v1. #96 reuses those exact bytes and enforces the
same-ordinal full M1/B1 pair; the I1-only latency pair compares only the exact
base manifest/digest and ignores M1's unrelated storage.

The frozen protocol does not claim nanosecond-exact operating-system wakes.
It fixes nominal monotonic starts 16,666,667 ns apart, a 2 ms maximum admission-
start lateness, exact 750,000,000 ns episode origins, and fail-closed
miss/drop/gap handling. The one actual-admission sample `A_i` starts latency
and checked-adds the absolute I1 Run deadline
`D_i=A_i+150,000,000 ns`; nominal `S_i` and the quiescence drain never extend
that budget, and missed or expired work cannot publish. Logical results use the
typed canonical
`ContentDigest`; raw little-endian payload, canonical manifest, semantic trace,
and golden identities remain separate. Every repeated M1 B1 occurrence carries
a distinct phase/cycle/job identity through charge, admission, output commit,
receipt, and evidence; corpus cycle never masquerades as retry attempt. M1
records distinct same-ordinal
isolated-I1 and isolated-B1-cap-8 pair digests in addition to the ordinary
candidate/reference baseline digest.

Evidence rows and bundles also have closed ASCII length-framed manifests. Their
fixed field order/types, explicit known-empty and N/A encodings, section/row/
bundle SHA-256 domain separators, digest self-exclusion, functionally unique
canonical row keys with exact item/row/bundle matching, and one exact target-row
selection per comparison/pair let an independent reader recompute every content
address. A candidate comparison digest first resolves exactly one retained
canonical five-field reference bundle, whose digest is independently rehashed,
whose workload matches, and whose complete functionally unique row list passes
canonical row resolution. Zero or multiple objects, five-field parse/schema or
rehash failure, wrong role/workload, or missing, duplicate, or mismatched target
rows invalidate every related reference-relative verdict. External
prerequisites, retained sections/provenance, rows, and bundles seal in address-
dependency topological order; direct or transitive self, enclosing, later-stage,
comparison, or M1 cycles fail closed. #93 through #96 may add their assigned
inner collector records but cannot redefine this v1 envelope, identity join, or
address DAG.

The delivery rows are fixed:

| Issue | Required target evidence |
| --- | --- |
| [#93](https://github.com/kevin-zf1123/photospider/issues/93) | I1 isolated latency, waste, memory, and required output correctness. |
| [#94](https://github.com/kevin-zf1123/photospider/issues/94) | I2 preview/final latency, Host/conditional-Metal residency and copy waste, memory, and required output correctness on the exact I1 lineage. |
| [#95](https://github.com/kevin-zf1123/photospider/issues/95) | B1 isolated throughput, exact determinism, fault-free zero waste, memory, and fixed storage/performance probe-to-schema, encoder, eligibility, and compatibility evidence at caps 1 and 8. |
| [#96](https://github.com/kevin-zf1123/photospider/issues/96) | M1 mixed latency, Throughput progress, fairness, waste, and memory using the exact I1/B1 fixtures and storage-compatible B1 pair without constraining its I1-only pair. |

ADR 0010 is the current accepted decision record, not a statement of current
runtime capability. The workloads, missing collectors, and valid evidence rows
remain downstream target work. Existing policy-order tests,
`BenchmarkService`, lifecycle telemetry, ledger snapshots, and the manual
OpenCV scaling tool do not by themselves establish profile conformance. The
maintained manual/release protocol and test-ownership boundary are documented
in [Testing and Validation](../development/Testing-and-Validation.md#execution-profile-slo-manualrelease-protocol).

## Server and Plugin Isolation

`photospiderd` remains a same-user local workstation sidecar. A network or
multi-tenant product uses a separate control plane, worker manager, constrained
`photospider-worker` processes, and durable artifact store.

The current operation plugin interface remains a provisional C++ ABI. Its
C-linkage registrar symbol and numeric handshake gate only the expected
interface generation; matching SDK/toolchain/runtime compatibility is still
required for the C++ values, callbacks, objects, and vtables that cross the
DSO. Policy plugins instead use the exact-layout pure-C ABI v1 and receive only
immutable scalar candidate snapshots, but they remain trusted in-process code.
A future operation ABI replacement, policy ABI generation, or isolated
invocation protocol is a separate versioned migration, not a compatibility or
security promise inferred from the current gates.

The `ExecutionService` sees isolated plugin execution through a
`PluginInvocationExecutor`. A separate `PluginRuntimeSupervisor` owns worker
processes, protocol, heartbeat, deadlines, restart backoff, sandbox/capability
policy, shared-memory or file-descriptor transport, quotas, and output
descriptor validation. The first isolated path targets CPU operation plugins;
cross-process GPU handles require a later device/fence protocol.

## Cross-Cutting Invariants

1. Only dispatcher-ready tasks enter the execution domain.
2. A Run publishes one terminal outcome and state transitions are monotonic.
3. Revision, supersession generation, and cancellation are checked before
   visible commit.
4. Queued, start, operation chunk, dependency release, completion, and commit
   paths observe cancellation where the operation contract permits it.
5. Deadlines use a monotonic clock. Non-preemptible kernels may overrun, but an
   overdue result cannot be presented as current.
6. Every reservation is released exactly once after success, error,
   cancellation, or worker failure.
7. Newly ready dependent work re-enters global policy rather than permanently
   bypassing fairness through local queues.
8. Graph close stops new-Run admission for that graph, preserves settlement for
   admitted Runs, and cancels or drains them; only process shutdown stops the
   whole execution domain after admitted work quiesces.
9. Third-party policy and plugin code cannot mint resource tokens or exceed
   host-owned quotas.
10. Terminal publication does not imply Run reclamation; all leases and grants
    must quiesce and release first.
11. `(RunId, RunLocalTaskId)` is the completion identity; a policy-binding or
    execution-route generation is not a Run identity.
12. Run success means validated Graph/RT publication or a valid no-op; it does
    not imply cache, output, Graph-document, daemon, delivery, or response
    completion.
13. Cache is discardable acceleration and never the durable user-output
    authority; cache persistence failure cannot rewrite an already successful
    Run or output commit.
14. Durable output commit uses stable identity, completely validated and
    synchronized payload/metadata and canonical manifest files, atomic
    no-replace manifest-last publication, leaf-to-durability-root directory
    barriers, and a typed achieved-durability receipt; recovery is idempotent
    and delivery is at least once, not exactly once.
15. Graph-document save remains a separately versioned transaction, and daemon
    terminal state or acknowledgement remains a non-durable transport
    observation.
16. A requested durability level that the platform cannot achieve fails
    explicitly and is never silently downgraded.
17. A caller or daemon may aggregate Run, output, Graph-document, cache/codec,
    and response facts into one request outcome, but it preserves each
    authority-owned fact and never projects composite failure back into Run
    state.

## Dependency Ordering

The architecture has a dependency order even though design work may overlap:

```text
dependency-neutral kernel
        ↓
ComputeRun and CPU execution domain
        ↓
general data and heterogeneous execution
        ↓
execution profiles, server runtime, and plugin isolation
```

The first executable vertical slice of each domain must preserve current Host
behavior and add durable tests before broader migration. Interface renames and
ownership transfers are completed without permanent compatibility wrappers,
in accordance with repository migration discipline. In particular, the
process execution domain must preserve the current bounded-admission error and
rollback guarantees while replacing per-graph physical worker ownership.
Issue #70 satisfied that rule by deleting the former counter and introducing a
checked multi-dimensional ledger; subsequent slices must extend that ledger
without restoring a second resource authority.
