# Policy and Execution Architecture

This document is the authoritative current description of how Photospider
chooses ready work and how it executes that work. Policy and execution are
separate ownership domains:

- a **policy** ranks immutable Host-admissible candidates and owns no resource;
- an **execution route** is a private Host implementation that owns physical
  queues, workers, devices, and completion adapters;
- a **Run** owns request identity, cancellation/supersession state, dependency
  progress, staged output, and its terminal result;
- the **Host** alone validates policy output, reserves resources, commits a
  start, and enters an executor.

The former worker-owning scheduler SDK, `IScheduler` hierarchy, per-Graph
physical owners, and scheduler plugin ABI are absent. There is no compatibility
adapter or forwarding API.

## Ownership Model

`ExecutionService` is the process execution-domain owner used by an embedded
Host. It owns:

- the bounded ready store and its complete ready-byte charges;
- one Interactive and one Throughput policy binding;
- process fairness and three-to-one class arbitration state;
- the fixed CPU worker pool, one service-owned Metal worker lane, a fixed
  `DeviceExecutorRegistry`, and private `serial_debug` and `gpu_pipeline`
  routes;
- Host-authored candidate, Graph, Run, entry-version, enqueue, snapshot, and
  selection identities;
- ready-to-execution resource exchange, exact implementation/exclusive-key
  gates, and in-flight callback ownership.

`GraphRuntime` stores only copied HP and RT route ids with nonzero generations.
It owns Graph state, compute/event/trace observation, and request serialization,
but it does not own a physical worker pool or policy-plugin context.

`ComputeRun` remains stable behind Run leases until every callback, dependent
release, completion publication, and staged commit contender settles. Policy
callbacks never receive a Run pointer or lease.

The resource lock order is:

```text
ExecutionService ready-store/service state
  -> Run state
    -> ResourceLedger reservation state
```

No policy callback executes while any of those locks, a Graph lock, or a policy
registry/binding lock is held.

## Policy Classes and Bindings

There are exactly two service classes:

| Class | Intended work | Built-in type |
| --- | --- | --- |
| Interactive | latency-sensitive work, optionally with a monotonic deadline | `interactive` |
| Throughput | weighted background work | `throughput` |

The process owns one binding per class. Even when both classes use the same DSO
type, they receive separate contexts and separate nonzero binding generations.
A same-name replacement still creates a new generation, clears the old
generation's fault, drains its active invocations, destroys its context once,
and retires its DSO lease only after the last dependent value is gone.

`configure_policy_defaults` prepares both candidate bindings outside the
publication lock and commits both or neither. `replace_policy` applies the same
prepare/publish/drain discipline to one class. A failed create, validation, or
publication leaves the previous binding and generation unchanged.

## Pure C Policy ABI v1

The only installed policy header is
`include/photospider/policy/policy_plugin_api.h`. It is self-contained under
C11 and C++17 and defines a natural-layout 64-bit ABI with exactly two exports:

```c
uint32_t ps_policy_plugin_get_abi_version(void);
ps_policy_status_v1 ps_policy_plugin_get_api_v1(
    ps_policy_plugin_api_v1 *out_api);
```

The API table contains four mandatory callbacks: metadata, create, select, and
destroy. The exact record sizes are:

| Record | Bytes |
| --- | ---: |
| `ps_policy_string_view_v1` | 16 |
| `ps_policy_type_metadata_v1` | 80 |
| `ps_policy_create_args_v1` | 40 |
| `ps_policy_candidate_v1` | 120 |
| `ps_policy_selection_snapshot_v1` | 64 |
| `ps_policy_decision_v1` | 48 |
| `ps_policy_plugin_api_v1` | 80 |

ABI v1 accepts exact sizes, kinds, alignments, offsets, callback pointers, enum
values, bounds, and zero-required reserved words. It has no tail-extension
rule. A new record shape requires a new ABI generation.

A policy receives only scalar candidate descriptors: opaque ids, deadline,
weight, trusted work and byte charges, projected Graph/Run service scores,
dispatch age, enqueue sequence, and flags. It never receives an executor,
worker, device, queue, allocation service, resource grant, Run, Graph,
completion route, logger, or lifecycle callback. Borrowed snapshot memory is
valid only until `select` returns.

The Host opens a DSO eagerly and locally, resolves and calls only the version
export before exact ABI equality, then validates the complete API and every
metadata row. One DSO is published as an all-or-nothing type-registration
transaction. Internal duplicates, conflicts, invalid UTF-8, noncanonical names,
reserved built-in names, invalid class masks, or malformed callback output
publish no row.

Active metadata, bindings, contexts, and invocations retain independent DSO
leases. Registry unload removes visibility but cannot invalidate an active
binding. An honest in-process callback that never returns has no timeout or
forced recovery guarantee; process isolation is a separate future boundary.

## Host-Authored Frontier

The Host chooses the service class before invoking a policy. When both classes
have scheduler-selectable work, it permits at most three consecutive
Interactive starts before one Throughput start. Scheduler-selectable means a
current ready lane head passes Run lifecycle, cancellation, operation-gate,
and physical-route eligibility for that worker; it deliberately excludes
transient execution child-grant capacity. Within the chosen class the Host
exposes at most one lane head per live Run.

Before a plugin sees candidates, the Host reduces them through these rules:

1. only current, scheduler-selectable, cancellation-safe, route-compatible
   lane heads are considered;
2. after eight same-class starts, only the maximum-age frontier remains;
3. otherwise Interactive work with the earliest finite deadline remains;
4. candidates outside the minimum projected Graph-service quantum are removed;
5. candidates outside the minimum projected Run-service quantum are removed;
6. score saturation escapes through the oldest stable enqueue sequence;
7. stable enqueue order is the final built-in tie-break.

The built-in policies use this same frontier and validation path as DSO
policies. The plugin may select one candidate from the immutable original
snapshot or abstain; it cannot widen the frontier or mint work.

## Decision Classification and Fallback

The Host first validates callback completion and every decision byte against
the original call: status, size, kind, reserved fields, decision kind,
generation echoes, and candidate identity. Only a valid original-snapshot
selection is then compared with current Host state.

There are two distinct outcomes:

- **obsolete by Host state**: the decision was valid when made, but readiness,
  cancellation, supersession, route, fairness, or generation state
  changed. The Host may take at most two fresh plugin snapshots, then uses the
  current same-class built-in choice. This records no policy fault.
- **invalid plugin decision**: the callback failed, threw a catchable foreign
  exception, abstained, returned malformed bytes, echoed the wrong generation,
  or named a candidate outside the original snapshot. The first fault is sticky
  for that binding generation, future calls bypass it, and successful
  replacement clears it.

Fault categories are `Abstained`, `CallbackStatus`, `CallbackException`,
`MalformedDecision`, `GenerationMismatch`, and `CandidateOutsideSnapshot`.
Optional Host snapshot allocation/bound failure is non-faulting and uses the
untruncated built-in path. A trusted built-in invariant violation fails only the
affected Run as `GraphErrc::ComputeError`.

## Reserved Start

A returned candidate id is not execution authority. The Host keeps a private
`SelectionPin` containing the original entry identity/version and rechecks
current state under the documented lock order. Before irreversible
ready/fairness mutation, `StartTransaction` stages the CPU, retained-memory,
and scratch child grant plus any first-use implementation/key gate rows.
Fallible gate allocation precedes active-counter mutation, and no-throw RAII
releases the staged grant on every rejection.

The remaining commit is allocation-free and non-throwing. It atomically:

- acquires the exact implementation count and nonempty exclusive key;
- removes the exact ready entry;
- exchanges its ready grant for execution grants;
- advances class, Graph, and Run service accounting;
- updates the Interactive burst count and in-flight state;
- transfers callback ownership to the selected private route.

No executor callback begins before that commit. Every rejection or exception
before commit preserves ready/fairness/burst/in-flight state and releases staged
grants and gate rows exactly once. Completion, cancellation, supersession,
dependency release, and Run settlement also release their owned state exactly
once. A started callback retains its operation gate until provider exit or
callback skip, even if cancellation or failure purges its queued siblings.

Temporary execution-grant exhaustion after revalidation is not a plugin fault
or obsolete-decision retry. The ready store marks the exact candidate/version
only for that worker's current cycle and recomputes class/frontier selection
without removing the entry, releasing its ready grant, or charging fairness.
This lets an independent lower-priority Run start from the remaining current
candidates. If every lane-compatible candidate is marked, the worker waits on
a predicate-protected notification epoch advanced by enqueue, dependency
release, completion/grant release, cancellation/failure purge, policy
replacement, and shutdown. Spurious wakes do not retry; a 50 ms low-frequency
fallback covers an otherwise unobservable external child-grant release, after
which cycle marks are cleared and current Host state is revalidated.

Only a successfully committed service start publishes evidence-startable
class facts. That observation cut rechecks the same ready/lifecycle/operation/
route predicates and additionally requires enough live child-grant capacity
for at least one candidate in each class. These capacity-aware evidence facts
drive M1 applicability only; they neither filter policy snapshots nor update
the three-to-one `consecutive_interactive_` state. A failed `try_grant()`
publishes no start observation.

An implementation cap or occupied exclusive key removes that candidate from
the scheduler-selectable frontier without exposing operation metadata to
policy plugins.
Worker retirement advances the same notification epoch when it releases the
gate. Direct sequential callers wait cancellation-aware without holding a
resource reservation, then acquire the same gate and one CPU/byte/scratch root
only around provider entry.

Every Host-owned retained operation or constraint key is charged by the actual
copied `std::string::capacity()` plus its null terminator. Full-plan admission
preallocates one independently owned, already charged constraint record per
logical task, including every tile, and moves each record exactly once into
that task's unique `ReadyTaskSubmission`. Dirty admission does the same for
each active task. Both freeze the complete shared estimate before moving any
record. Connected-preflight callable/submission copies and the direct lease are
charged independently. The operation gate stores only a borrowed
`string_view` erased before the stable owner retires. Queued work borrows its
`QueueEntry`-owned submission; direct acquisition copies the caller constraints
into its lease state before querying the gate, and its wait predicate, start,
and finish all borrow that state-owned copy. The caller input therefore need
not outlive the returned lease. This neither duplicates nor undercounts string
ownership. Checked terminator overflow and a one-byte-short retained limit fail
before provider entry without gate or ledger residue.

## Private Execution Routes

The route vocabulary is closed:

| Route | Ownership and behavior |
| --- | --- |
| `cpu` | Host-lifetime fixed CPU worker pool with reusable multi-entry execution; exposes CPU only |
| `serial_debug` | CPU worker zero with one callback in flight; exposes CPU only |
| `gpu_pipeline` | the same fixed CPU pool for CPU fallback plus one service-owned Metal lane; exposes Metal then CPU when the fixed registry owns a Metal executor, otherwise CPU only |

`heterogeneous` is not an alias. Execution routes are not plugins and cannot be
scanned or loaded.

`HostExecutionConfig` controls future-session HP/RT route ids and a worker
request in `[0,8]`. Zero selects bounded automatic resolution. Once the process
CPU pool is fixed, zero or an equal request preserves it; a different positive
request is rejected. Existing Graph sessions keep their route bindings.

`replace_execution` validates a closed-vocabulary route, prepares the new
ownerless binding, serializes against active same-session requests, and
publishes a new nonzero generation. A same-name replacement also advances the
generation. Failure preserves the old route.

Operation selection freezes one coherent callback, metadata, `Device`, and
nonzero implementation revision before Run admission. Planning retains only
the callback-free identity/metadata/shape, and submission must re-resolve the
same identity before it may retain the callable/DSO lease. Full HP, dirty HP/RT,
and connected-parameter preflight all consume the same canonical route-aware
inventory; full-task cache identity includes that inventory and the registry
generation. Region propagation and dirty TensorSlice eligibility use that same
request inventory and the matching HP/RT intent to select the actual
revisioned `OpImplementation` before testing source-private core identity. They
do not use a scalar-only lookup or filter away a selected same-key device
candidate. HP TensorSlice planning immediately converts every accepted
executable target/upstream selection into a callback-free operation key plus
the complete identity/device/shape/metadata route, then releases the temporary
callable/DSO lease. Dirty preparation compares those frozen routes with the
active task-population routes before ROI mutation, task materialization,
callable resolution, or admission; an empty active view returns before frozen
context comparison, while any remaining active route or context mismatch is
`NoOperation`. Final callable re-resolution remains mandatory.
Connected-preflight preparation also freezes
each callable/DSO lease and complete service root without entering provider
code; only an installed Run may perform reserved start and invoke the provider,
after which output-dependent dirty planning remains Run-owned. Every ready
submission carries the frozen device, and `ExecutionService` rejects a device
outside the configured route/registry inventory before publishing the Run.
CPU submissions enter the fixed CPU pool; Metal submissions enter the single
GPU lane and then the matching registry executor. Both lanes share
the common ready store, policy decision, reserved-start transaction, Host
ledger, Run maximum-parallelism grant, operation implementation/key gates,
cancellation, completion, exception, reuse, shutdown, and drainage rules; no
second device-capacity authority or per-Graph executor is created.

Full HP, dirty HP/RT, connected preflight, initial ready work, and
dependency-released work all enter the common ready-store, policy,
reserved-start, private-route, and Run-lease completion path.

V-6 adds no configured execution route and no second ready store.
`ReadyFence::async_wait` accepts a shared injected executor that must enqueue
rather than invoke inline. Its preconstructed continuation retains the executor
while pending or queued, then transfers that owner to callback-local retention
on entry. This keeps temporary executor ownership alive through callback
completion or exception unwinding while releasing an executor-owned queue
self-reference. Fence state and the source-private `ValueTransferTask` own no
worker or queue. The repository fake executor is a deterministic, thread-safe
test mechanism only; C++17 mutex/condition-variable rendezvous exercises real
registration/publication, cancellation/callback-entry, and
transfer-destruction/callback-entry races without sleeps.

V-7 adds a source-private, fixed `DeviceExecutorRegistry` to the same
`ExecutionService` domain. When the repository Metal plugin is enabled, the
Apple entry owns one device and command queue, supplies an invocation-scoped
texture/buffer allocator, and keeps a validated process-lifetime pipeline
cache. Reserved-start workers enter that
executor synchronously and invoke the already selected operation through the
same Run completion/exception path. The Metal Perlin provider now borrows those
resources and keeps no static native state; `GraphRuntime`, `Kernel`, operation
metadata, and policy state expose no native handle or capability hook.

V-8 adds source-private, explicit CPU/Metal access and residency to that same
execution domain. `AccessPlan` selects exactly one of direct, map, import,
transfer, or unsupported without performing hidden work. A transfer preserves
the logical `ValueRevisionId` while publishing a distinct checked
`StorageBinding`; CPU-to-Metal upload and Metal-to-CPU readback are explicit
asynchronous tasks. The Metal Perlin provider publishes a pending native Value
and encodes texture-to-shared-buffer readback without waiting on a command
buffer or calling synchronous `getBytes`.

`ExecutionService` owns the one process `ResidencyManager` and routes pending
Value continuations through its existing ready store. Exact completion
identity includes Graph/revision/target/intent/generation, Run/task, source and
destination revision, producer, and binding facts. Freshness admission,
source/destination terminal publication, and resident insertion form one
manager-locked transaction. Each source or destination producer supplied for
that publication must also retain the exact same non-null private `ReadyFence`
control state as its corresponding pending Value; matching revision, producer,
allocation, binding, or other scalar facts cannot substitute for this terminal
authority. A capability mismatch is rejected without consuming the rightful
admission, settling either fence, inserting residency, or releasing the
retained resource owner, so the exact admitted producer pair can retry. A late,
duplicate, or mismatched completion
cannot publish Ready, release dependent work, enter residency, or restore an
older commit right; it settles the affected destination with a typed failure
when it still owns that producer. Run settlement retains the executor and
continuation until every pending fence callback has retired. V-8 adds no
second ready store, Graph authority, persistence path, or device-memory
capacity authority. Settled replicas may remain reusable after Run release,
but the manager's 64-entry default bounds strong native/provider retention by
releasing the lowest-revision entry under publication pressure; generation
assignment alone does not bulk-clear them. This entry count neither measures
nor admits bytes.

The source-private I2 verification path additionally has an exact resident
release operation. Under the manager mutex it validates one nonzero revision,
the complete `StorageBinding`, and the producer identity, then extracts only
the matching map node; destruction of the retained Value/native owner occurs
after unlocking. A wrong identity is a no-op. This narrow operation does not
broad-clear residency, use capacity pressure as cleanup, or alter normal
lookup, publication, replacement, capacity, and eviction behavior.

V-9 places authoritative device-memory and scratch admission in the existing
service `ResourceLedger`, not in policy or residency. Each configured
non-CPU `DeviceId` has isolated limits. Metal atomically reserves native
size/alignment plans before allocation, audits `allocatedSize`, and commits
actual bytes before command submission. Persistent memory follows the native
Value owner through residency; scratch follows exact command completion.
Policy sees no native handle or token, does not rank byte owners, and gains no
second waiting/fairness queue.

Freshness publication uses two phases. Kernel first asks `ExecutionService` to
pretrack the lineage without assigning a managed current identity; this
fallible allocation completes before coordinator submission. When the
candidate is accepted as current, the coordinator invokes a no-throw,
no-allocation service callback while holding its mutex and before publishing
its own current row. That callback assigns the manager's exact accepted
generation under the manager mutex, including a coordinate-authorized numeric
decrease. Failed, close-rejected, and born-stale candidates never invoke it. A
stale Run that starts afterward cannot replace this coordinator-published exact
identity.
Standalone manager lineages, which do not use this callback, separately retain
numeric-maximum generation order.

## Compute I/O Execution Boundary

`ExecutionService::PoolState` owns one source-private `ComputeIoExecutor` with
one independent worker. Under one mutex, a passing limit check provisionally
reserves both the task count and a positive estimated-retained-byte amount
before lazy payload construction, queue publication, filesystem mutation, or
codec entry. Factory throw, empty callback, or task/queue-entry allocation
failure rolls that reservation back without an Accepted event. Successful
nonempty construction reaches one binary final decision: queue ownership and
Accepted publish together while admission remains open, or external shutdown
publishes Accepted atomically with its exactly linked Cancelled settlement
without callback entry. Each accepted task retains an explicit transaction
lifetime token and returns a typed completion. Success, failure, queued
cancellation, running late cancellation, construction rollback, and graceful
shutdown converge on exactly-once account release.

The executor authors immutable attribution events at those same accounting
linearization points. Admission records a monotonic nonzero sequence, typed
decision, exact charged task/byte delta, and same-lock process snapshot;
settlement links back to that admission and records the exact released delta
plus its same-lock snapshot. Rejection has zero delta. Process snapshots can
include unrelated concurrent users and sequence gaps are valid for a consumer
that observes only a subset; neither fact weakens the exact per-task proof.

The worker and completion boundaries prevent identity-specific self-blocking.
While admission remains open, the owning I/O worker's nested submission returns
inactive `InvalidRequest` before either budget or the lazy factory changes; a
concurrent admission stop retains `ShuttingDown` priority. The owning worker may
copy an already terminal completion, but a nonterminal completion wait fails
before condition-variable blocking. A completion keeps only a weak executor
identity for that comparison. Submitting to and waiting for another independent
executor remains legal.

Lazy factory invocation uses an allocation-free, exception-safe thread-local
scope stack. `shutdown()` rejects a target found anywhere in that stack before
changing `accepting`/`stopping`, acquiring join authority, or waiting for the
worker. This covers direct factory re-entry and indirect
`A factory -> B factory -> A shutdown` without rejecting an unrelated executor.
An external shutdown still stops admission and waits for every already charged
factory. A factory that returns after the stop produces the existing
Accepted/Cancelled submission and exact settlement; a factory that throws
performs exact rollback. Worker join completes only after construction
settlement and FIFO drain.

The first production vertical is staged HP cache save. `GraphCacheService`
still chooses eligibility, paths, precision, codecs, and error interpretation.
After the existing live lifecycle, supersession, and revision predicates,
graph-state policy submits the mechanism callback and waits for it before the
existing no-throw Graph publication. CPU compute workers are forbidden from
synchronously waiting for this completion, so a blocked cache codec does not
occupy the CPU execution domain. Because the current image-codec API is
indivisible, its whole I/O-facing call runs on the I/O worker; a future split
API must return independently admitted CPU-heavy phases to the CPU executor.

V-15 adds a second bounded user without changing that mechanism. The
source-private OpenEXR deep adapter submits one complete indivisible
single-part deep-scanline read or write as the callback. Before
`ComputeIoExecutor::try_submit`, one shared source-private path check maps an
empty or embedded-NUL `std::string` to the existing inactive `InvalidRequest`
fact. It performs no budget charge, lazy construction, path/Value/registry
capture, hook or worker entry, filesystem access, or OpenEXR call; therefore a
C-string filename API cannot silently select the NUL prefix. The direct write
preflight reuses this contract and throws the Host-owned adapter
`InvalidRequest` category.

For valid paths, executor admission receives a positive retained-byte estimate
and occurs before path capture, Value/provider generation retention,
result-state construction, filesystem effects, or OpenEXR entry. Once
accepted, the task retains its transaction token, copied path, exact provider
generation and input Value or decoded-result state through the complete codec
call. OpenEXR is invoked with `numThreads=0`, so the executor's one worker
remains the only adapter-created execution lane. Running cancellation cannot
preempt foreign codec code; it suppresses late result publication and still
releases task/byte accounts exactly once.

After generic Value inspection, the write path crosses a source-private
continuation barrier before it prepares an OpenEXR Header/frame buffer or
opens the output path. The barrier validates both signed windows, logical-site
and row-width arithmetic, exact inclusive `Box2i` coordinate representation,
and the `int` scan-line count consumed by `writePixels`. Only the continuation
released by that complete preflight may prepare or open the output. A typed
shape rejection therefore preserves an existing destination byte-for-byte and
does not create a missing destination.

This is a mechanism boundary, not a fourth scheduler or a persistence
authority. It adds no execution route, ready store, Graph owner, policy
decision surface, Host/device ledger dimension, or public ABI. Synchronous
cache administration/load, Graph-document operations, daemon job state, and
the private `OutputStore` remain unchanged. The executor owns no user path,
retry, overwrite, receipt, or durability policy. No current component provides
a crash-durable user-output commit, and ADR 0009's post-publication independent
cache outcome remains future work.

## Host, CLI, and IPC Surfaces

The public Host has eight policy operations and six execution operations. Its
final non-destructor virtual inventory is 58. Policy discovery and bindings are
process-scoped; execution info/replacement and execution trace are session-
scoped copied values.

`graph_cli` exposes:

```text
policy list|get|set|scan|load|plugins|help
execution list|get|set|help
```

Configuration uses `policy_dirs`, `policy_interactive_type`,
`policy_throughput_type`, `execution_hp_type`, `execution_rt_type`, and
`execution_worker_count`. Removed `scheduler` commands and `scheduler_*` keys
are rejected without translation.

IPC protocol version 2 replaces the old method family with eight `policy.*`
and six `execution.*` methods, including non-destructive `execution.trace`.
The daemon advertises exactly 60 sorted unique methods. Protocol version 1 and
old method names are rejected before Host access. The exact schemas and bounds
are maintained in
[`IPC-Protocol-v2.md`](../codebase-structure/IPC-Protocol-v2.md).

## Observability and Lifecycle Proof

Execution trace pages contain copied sequence, epoch, node, worker, action, and
timestamp values. Pages are non-destructive, bounded to 4,096 entries, and
preserve drop/exhaustion semantics. Trace data carries no queue or callback
capability.

`ExecutionService` also owns source-private
`ExecutionLifecycleTelemetry`: a schema-versioned fixed ring of 65,536 records
with non-destructive 1..4,096-record snapshot pages, atomic cuts, explicit
cursor gaps, and saturating cumulative drop accounting. Its 15 post-transition
counters combine registry state with exact ready entry, entered operation
callback, live root reservation, live child grant, policy invocation, and
current/displaced policy-binding ownership. Records contain copied scalar
identities only and expose no label, path, pointer, callback, lease, or mutable
handle. This store is not added to Host, CLI, or protocol v2.

`RunLifecycleRegistry` now drives Graph-close and process-shutdown
cancellation. Shutdown keeps already admitted ready/execution/completion paths
alive until every Run settles, then joins physical workers and retires policy
bindings before publishing `ServiceStopped` with all 15 counters zero. A
nonreturning callback can therefore keep shutdown honestly blocked; it is not
made recoverable. A same-service worker or policy caller is rejected by a
mutation-free preflight; after Kernel closes its publication gate, unexpected
transition failure is fail-stop because that gate cannot reopen. General-data
heterogeneous execution belongs to Issue #77;
process-isolated plugin supervision belongs to Issue #91.

Registry mutations and the registry-derived portion of `WorkerJoined` and
`BindingRetired` records are serialized by the same lifecycle fence. Those
physical-retirement records therefore carry one exact nine-counter registry
cut, while ready, entered-callback, root, child, policy-invocation, and binding
ownership are sampled independently. The M1 evidence replay reconstructs
Graph/candidate/bundle/Run/generation causality and exact-checks the nine
registry counters at every event and page cut. It validates only capacity and
ownership reachability for the six physical samples, including resources held
by a pending candidate before bundle publication; it never fabricates exact
physical deltas from an event kind. Final M1 evidence must terminate with
`ServiceStopped` and all 15 counters zero.

## Current Execution-Profile Evidence and Limitations

Built-in policy behavior by itself is not an execution-profile SLO. The paths
have deterministic weighted ordering, eight-dispatch aging, a three-to-one
class-start bound, and Interactive headroom. Maintained tests prove those
mechanisms and exact resource release. ADR 0010 separately defines latency,
throughput, fairness, determinism, waste, and memory as independent verdicts
over four immutable workloads.

Issue #93 now implements the isolated `I1-edit-storm-v1` mechanism and inner
evidence path. A source-private `I1Host` submits the exact HP request through
the ordinary embedded asynchronous Host, InteractionService, Kernel,
supersession, and `ExecutionService` path while supplying explicit Interactive
QoS, weight one, cap eight, and the immutable per-edit deadline. A read-only
`ComputeRunObservationSink` records current generation, physically committed
service start with `(RunId, RunLocalTaskId)` and charge, accepted cancellation,
current-visible output, terminal outcome, Run quiescence, exact root-resource
return, and caller-visible future plus Host-tracking settlement. Each product
transition reserves a coordinate from the same request-scoped causal sequence
at its linearization point. In particular, logical service-start commit and
cancellation acceptance are ordered before either callback delivery. The sink
grants no scheduling, cancellation, ledger, graph, or commit authority and is
not an installed Host, IPC, CLI, policy-plugin, or operation-plugin contract.
After the live HP Graph swap, the sole commit contender emits current-visible
output and the succeeding terminal-success observation in one Run-arbiter
resolution; a rejected or already-resolved contender emits neither event.

Before the final I1 Host call, the collector reserves the typed accepted-row
coordinate `(A_i, event_sequence_i)` and carries it through the source-private
Host/Kernel request seam. Kernel binds that exact coordinate into
`SupersessionIdentity`, and `ComputeRequestCoordinator` publishes and observes
the complete current identity rather than reconstructing it from a later
callback. For coordinate-bound I1 lineage, replacement requires only a
strictly newer accepted coordinate; equal timestamps are ordered by the
row-local accepted sequence. Generation remains nonzero and unique but records
preparation arrival, so a coordinate-authorized publication may carry a lower
generation. Mixed or unbound traffic remains generation-ordered. The native
freshness manager adopts the exact coordinator-published generation and does
not let a stale numerically higher Run observation restore itself. The
observation sink's causal sequence is a separate allocator and ordering domain
that also starts at one. Current-generation evidence copies the product-bound
accepted coordinate, and the evaluator requires an exact row-to-product
binding, unique nonzero generations, and strict product-coordinate order. A
failed Host call may retain the proposed coordinate for diagnostics, but it
cannot create an accepted row, current observation, or product binding.

The ordinary public request and the source-private I1 request use the same
embedded-Host preparation transaction. Caller promise/future ownership, the
successful result envelope, a one-delivery backend bridge, the status worker,
and close-visible tracking are all established before `InteractionService`
enters Kernel. Kernel may publish current concurrently before returning, so the
accepted Host tail is deliberately no-fail: it only shares the backend future,
delivers it through the prebuilt bridge, and moves the prebuilt result. A
deterministic source-private test seam fails at the last pre-Kernel point and
proves that Host resource failure exposes no current generation or product
output; it does not alter installed Host, IPC, CLI, or plugin contracts.

I1's curve arithmetic is frozen rather than delegated to an OpenCV bulk
reciprocal approximation. Each coefficient is rounded once to binary32 RNE,
and every sample uses `RNE32(1/RNE32(1+RNE32(input*k32)))`. The provider saves,
installs, and restores the worker floating-point environment around those
explicit scalar cuts. An independent oracle versioned
`i1-coordinate-pattern-curve-chain-fp32-v1` reconstructs the source and four
stages without Host/Kernel/cache/scheduler/YAML/provider dependencies. For the
HWC `[2048,2048,4]` NativeScalar32 tensor and frozen ImageFacet, its exact
`Sha256CanonicalV1` digest is
`17266cf3871544d61decc0805ce300ded59a688e75e826c15ce4b6989db4c493`.

The frozen I1 graph, twelve coefficients/Regions, success-only accepted
coordinate collector and product binding, continuous cold/warmup/measured
221-slot grid, tie and guard rules, canonical DenseTensor output digest,
resource snapshots, and
fail-closed episode/replicate evaluator are current. `ResourceLedger` Host and
device snapshots now retain lifetime high-water values for successful
reservations as well as current/limit values. At `Q_end`, I1 first captures the
first-excluded causal coordinate; required terminal/quiescence/resource/Host
settlement belongs only when its timestamp is no later than the boundary and
its sequence precedes the cut. A later resource/lifecycle snapshot proves
eventual exact return but cannot backdate settlement into that history. The
closed `execution-profile-i1-inner-row-v1` evidence independently evaluates latency,
waste, memory settlement, and output correctness. It does not claim the ADR
0010 canonical 15-field outer row, section, bundle, reference comparison, or
the profiles assigned to Issues #94 through #96.

The expected digest is installed before candidate execution and must equal
that immutable oracle; absence or substitution is Invalid, while a complete
candidate mismatch is Fail. Each visible `Value` is traversed once before
`Q_end`, its typed result is frozen, and the handle is released. Evaluation and
JSON therefore cannot rehash the payload. One owned Value-free evaluator may
overlap next-baseline preparation but must complete before admission; JSON,
dump, and durable ordered flush wait for `T^I1` or an abort that revokes later
submission. The live evidence set is bounded by one evaluator and 221
Value-free rows, with exceptions returned through the sole future.

The manual `i1_edit_storm_benchmark` is excluded from the default build and
CTest. It executes the exact 221-slot workload and writes raw inner rows plus a
replicate summary to an explicit disposable directory outside the checkout.
Building the harness or running deterministic tests is not a machine
conformance result: an I1 claim requires a complete valid exact-cadence run and
retained evidence, and this document makes no Interactive, batch/render/
testbench, or mixed-profile conformance claim. `BenchmarkService` and
`opencv_operation_concurrency_benchmark` retain their narrower legacy metrics
and are not canonical execution-profile evidence.

The target contract deliberately reuses legal current descriptor values rather
than inventing an execution-profile enum. I1 is
`GlobalHighPrecision`/`Full`; I2's realtime request lineage carries an
`Interactive`-quality `RealTimeUpdate` preview child and a `Full`-quality
`GlobalHighPrecision` final child, each with explicit Interactive QoS. The
current #94 implementation carries optional source-private progressive options
through embedded Host, Kernel, and ComputeService. A successful current RT
preview publication arms one cancellation-ordered gate, emits the observation-
only final-trigger coordinate, and submits the HP child immediately afterward;
supersession or cancellation can deny that gate, and ordinary realtime requests
retain the previous concurrent behavior when the option is absent. This state
machine is not a public Host, IPC, CLI, or plugin API. Required logical equality
uses
`compute_content_digest(Value)` and the typed
`Sha256CanonicalV1` `ContentDigest`, not raw storage bytes.

The source-private I2 profile and evidence evaluator implement the frozen
111-slot grid, twelve-edit admissions, child descriptors, publication ordering,
Host acquisition, conditional real-Metal residency, lifecycle/resource
settlement, and four independent inner verdicts. Evaluation requires the sole
accepted current-generation observation to precede every matching child event;
each Cancelled terminal must have exactly one descriptor-identical earlier
cancellation, while every non-Cancelled terminal must have none. Missing,
duplicate, late, extra, or drifted evidence invalidates all four verdicts. The
Host settlement for each edit must in turn have a sequence strictly greater
than every materialized child resource settlement and a steady timestamp no
earlier than any of them. Its status is successful exactly when at least one
child materialized and every materialized child Succeeded: preview-only and
preview-plus-successful-final succeed, while preview-plus-cancelled-final and
no-child fail. A sequence, time, or status contradiction makes all four axes
Invalid without inventing a child outcome. After copying second-Metal-reuse,
diagnostic, resource, and no-I/O facts, the Host performs the exact row-scoped
resident release before its final snapshot; every configured device's complete
memory-and-scratch `reserved` vector must equal the pre-row baseline. The
output axis independently requires the caller-supplied expected preview and
final digests to equal `i2_frozen_preview_content_digest()` and
`i1_frozen_final_content_digest()` before candidate comparison. Expected
evidence corruption is Invalid even when candidate evidence mirrors it; a
candidate-only mismatch with intact expected oracles is Fail. At replicate
level, memory and output consume all 111 rows. Latency and waste consume samples,
service, and complete verdicts only from measured slots `11..110`; cold and
warmup propagate Invalid only, so their Pass or Fail values cannot pollute the
100-row steady-state aggregate. The manual
`i2_progressive_benchmark` target is `EXCLUDE_FROM_ALL`, is absent from CTest,
and writes `execution-profile-i2-inner-row-v1` evidence only to a caller-selected
directory. That inner record is not the canonical ADR 0010 15-field outer row,
bundle, or reference comparison. Building it or passing deterministic tests is
therefore not an I2 machine-conformance claim; the exact 111-slot workload must
be invoked explicitly and retained before any such claim.

Issue #95 now supplies the source-private B1 composition without changing an
installed surface. `B1Host::compute_b1_image` carries exact Throughput QoS,
weight one, the selected cap, and an observation-only sink through the same
embedded Host, Kernel, provider, ledger, and `ExecutionService` path used by
ordinary synchronous compute. The same private view exposes the one real
process `ComputeIoExecutor` and authority-free execution snapshots; it creates
no second scheduler, worker pool, ledger, Graph authority, or public request.

`B1OutputStore` is the B1 manual/release output owner, not the still-target
general product `OutputStore` from ADR 0009. Under one preselected canonical
root it retains a no-follow root descriptor, holds a nonblocking advisory
exclusive lock for its lifetime, and creates a mode-`0700` private staging
anchor/slot. Cooperating processes and threads must honor that lock and reserve
the B1 staging/occurrence names to this single owner. It records the named
directory identity before `openat` and requires the held descriptor to match
before any artifact write, then submits the exact 67,108,864-byte payload charge
and exact manifest charge as two ordered tasks to the process executor. After
both settle, the store atomically renames the complete private slot to the
immutable public occurrence with platform no-replace semantics and synchronizes
both namespaces. Every artifact mutation, barrier, and revalidation remains
descriptor-relative, so pathname or real-directory slot replacement cannot
redirect writes. Before public rename, the guard settles accepted work before
private cleanup, checks recorded leaf/directory identities twice, checks each
name-removal result and following absence, synchronizes parents, and fail-stops
on detected unowned residue or cleanup failure. POSIX does not atomically bind
the final identity check to `unlinkat`/`rmdir`; this guarantee therefore relies
on the cooperating exclusive-owner precondition and makes no claim about
arbitrary non-cooperating same-UID mutation in that interval. A pre-guard anchor
handoff failure preserves ambiguous residue without a retryability claim. Only
checked private removal and observed absence inside the precondition keep the
commit identity retryable from an empty namespace. Atomic public rename changes
the guard to public-pending and revokes deletion authority. Later barrier,
final-validation, or receipt failure preserves the occurrence and empty anchor;
same-commit retry descriptor-relatively verifies exact payload/manifest bytes
and identities, finishes missing barriers, and returns the same receipt without
new output work or rewriting. A non-directory or real directory with no
transaction-looking leaf (empty or containing only unknown markers) is plainly
foreign and returns `SlotExists` untouched. Once payload, manifest, or private-
manifest residue is present, an incomplete/extra entry set or byte/identity
drift returns `RevalidationFailed` untouched. A reconciled receipt carries an
empty `io_observations` sequence because no new tasks ran; it cannot fabricate
the current B1 FSM, so evaluation requires the retained earlier new-work stream
and fails closed when that stream is unavailable. The store writes tight
little-endian RGBA binary32 bytes,
syncs and revalidates the payload and manifest, publishes once, completes
leaf-to-root directory barriers, and only then returns a typed crash-durable
receipt. That receipt has no public field-based constructor: only the store can
mint its immutable typed fields after revalidation. The store can also retain
an opaque root-authority capability backed by a duplicated descriptor that
shares the same open-file description and advisory-lock lifetime. Copies held
by evaluated inner rows therefore keep the descriptor and exclusive ownership
alive even after the originating store is destroyed. Every offer and
settlement retains the complete occurrence/task identity and executor-authored
exact delta plus same-lock I/O snapshot; capacity retry keeps attempt zero and
the same charge. A passing limit check is only a provisional constructing
reservation: factory throw/empty or task/queue-entry allocation failure mints no
Accepted event. Successful construction publishes Accepted either with queue
ownership or, if external shutdown won, atomically with its exactly linked
Cancelled settlement. Every active snapshot task
occupies exactly one constructing/queued/running phase; retained global event
sequences may have numeric gaps for omitted unrelated work, but task-local
transitions may not be missing. Planned bytes and per-task events are
authoritative only for
Compute I/O admission, high-water, and exact task settlement, not physical
memory ownership, durability, RSS, or ledger/device evidence.

The source-private B1 profile, environment validator, and evidence evaluator
also implement the immutable 34-seed logical/raw golden table, canonical
semantic trace, exact 21/24/4-field environment schemas, raw backend/mount/
performance proof mappings, eligibility/root-containment/compatibility, and
four independent inner verdicts. Applicable evidence and JSON retain the raw
storage proof as the one closed canonical six-field expected document,
including all 21 raw field observations, mount inputs, two performance cuts,
transaction/receipt events, and root/destination observations. No derived
proof boolean is retained. Every compatibility side reparses those bytes,
reruns all mappings, and recomputes eligibility from its own canonical storage
bytes before exact-matching the retained claim. It then independently binds
that expected claim to an opaque source-private actual capability. Only a live
held-root descriptor capability, immutable store-minted typed receipts, and a
trusted live probe adapter can mint its inputs; public aggregates, copied
values, and retained proof bytes cannot. Every validation call obtains a fresh
root/receipt/probe snapshot from that live observer. The complete raw probe is
an observation result rather than minting authority, and copies inside
`B1InnerRowInput`/`B1InnerRow` share the observer and extend its source lifetime.
JSON adds a readable decoding and diagnostic initial-snapshot digest but no
alternate proof grammar and no reusable authority. Missing
trusted observation for any external storage declaration makes that side
machine-ineligible; copying the retained proof into the actual-observation path
is forbidden.
`b1_immutable_benchmark` is
`EXCLUDE_FROM_ALL`, absent from CTest, and executes one exact 34-job inner row
below a caller-selected root. Its four environment files are expected input,
not observation authority. The current portable Darwin/Linux path observes the
held root and real receipts but cannot independently verify all mount,
performance, hardware-cache, power-loss-protection, and transaction-event
facts, so it emits an Invalid row rather than a machine-conformance claim until
a trusted complete probe is available. Building it, showing its help, or
passing deterministic tests is not a B1 machine-conformance result; this
document claims neither an exact three-replicate machine run nor the #96 outer
row/bundle/reference composition.

## Implementation and Validation Entry Points

- `include/photospider/plugin/op_contract.hpp`
- `src/lib/core/ps_types.hpp` and `.cpp`
- `src/lib/compute/task_graph_planning.hpp` and `.cpp`
- `src/lib/compute/compute_task_submission.hpp` and `.cpp`
- `include/photospider/policy/policy_plugin_api.h`
- `src/lib/policy/policy_registry.hpp` and `.cpp`
- `src/lib/compute/execution_service.hpp` and `.cpp`
- `src/lib/compute/run_lifecycle_registry.hpp` and `.cpp`
- `src/lib/compute/execution_lifecycle_telemetry.hpp` and `.cpp`
- `src/lib/benchmark/i1_host.hpp`
- `src/lib/benchmark/i1_profile.*`
- `src/lib/benchmark/i1_evidence.*`
- `src/lib/benchmark/i2_host.hpp`
- `src/lib/benchmark/i2_profile.*`
- `src/lib/benchmark/i2_evidence.*`
- `src/lib/benchmark/b1_host.hpp`
- `src/lib/benchmark/b1_profile.*`
- `src/lib/benchmark/b1_environment.*`
- `src/lib/benchmark/b1_output_store.*`
- `src/lib/benchmark/b1_evidence.*`
- `src/lib/compute/progressive_compute.*`
- `src/lib/core/exact_box_downsample.cpp`
- `src/lib/runtime/resource_ledger.*`
- `src/lib/execution/compute_io_executor.*`
- `src/lib/adapters/openexr/openexr_deep_scanline_adapter.*`
- `src/lib/execution/execution_task_runtime.hpp`
- `src/lib/execution/device_executor_registry.*`
- `src/lib/execution/metal_device_executor.{mm,stub.cpp}`
- `include/photospider/memory/ready_fence.hpp`
- `src/lib/execution/value_transfer_task.*`
- `src/lib/runtime/graph_runtime.hpp` and `.cpp`
- `src/lib/runtime/kernel_execution_facade.cpp`
- `src/lib/graph/graph_cache_service.*`
- `src/lib/ipc/output_store.*`
- `include/photospider/host/host.hpp`
- `src/lib/host/embedded_host.cpp`
- `src/lib/ipc/{codec,client,host,request_router}.cpp`
- `tests/unit/test_policy_registry.cpp`
- `tests/unit/test_compute_io_executor.cpp`
- `tests/integration/test_openexr_deep_scanline_provider.cpp`
- `tests/unit/test_compute_run.cpp`
- `tests/unit/test_progressive_compute.cpp`
- `tests/unit/test_i2_profile.cpp`
- `tests/unit/test_i2_evidence.cpp`
- `tests/integration/test_i2_product_path.cpp`
- `tests/verification/i2_progressive_benchmark.cpp`
- `tests/unit/test_i1_profile.cpp`
- `tests/unit/test_i1_evidence.cpp`
- `tests/integration/test_i1_product_path.cpp`
- `tests/verification/i1_edit_storm_benchmark.cpp`
- `tests/unit/test_b1_profile.cpp`
- `tests/unit/test_b1_environment.cpp`
- `tests/unit/test_b1_output_store.cpp`
- `tests/unit/test_b1_evidence.cpp`
- `tests/integration/test_b1_product_path.cpp`
- `tests/verification/b1_immutable_benchmark.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_metal_device_executor.cpp`
- `tests/integration/test_ipc_daemon.cpp`
- `tests/integration/dependency_disabled_install_smoke.py`
- `tests/integration/static_product_consumer_smoke.py`

See also [Compute Flow](Compute-Flow.md),
[Compute Boundaries](Compute-Boundaries.md), [Plugin ABI](Plugin-ABI.md), and
[Graph Lifecycle](Graph-Lifecycle.md).
