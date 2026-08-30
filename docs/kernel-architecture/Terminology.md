# Kernel Terminology

This glossary defines the language used by the current kernel implementation.
Terms that remain only in an accepted target decision or the
[kernel evolution target](../roadmap/Kernel-Evolution.md) must not be described
as current runtime objects. The Issue #76 ownership defined by
[ADR 0007](../adr/0007-compute-runs-and-process-execution-have-separate-owners.md)
is current where this glossary says so.

## Product and Runtime Ownership

**`ps::Host`**
The only supported interface for code outside the backend. Embedded frontends,
the CLI, and the IPC adapter exchange copied public values through this seam.
They do not obtain `Kernel`, `GraphRuntime`, or `GraphModel` references.

**Embedded Host adapter**
The in-process implementation of `ps::Host`. It owns a `Kernel` and internal
interaction state while sharing the process-wide operation plugin owner.

**IPC Host adapter**
The installed client-side implementation of `ps::Host`. It translates Host
calls into the versioned local IPC protocol and owns client polling plus
temporary mapped named-Value archive delivery lifetimes. Returned Values use
fresh detached runtime storage; the adapter owns neither daemon sessions nor
backend runtime objects.

**`Kernel`**
The internal multi-graph facade and composition owner for graph, compute,
cache, traversal, inspection, and persistence services.

**`GraphRuntime`**
The per-graph resource container. It owns one `GraphModel`, one bounded
graph-state lane, one bounded private compute-request lane, one private
supersession coordinator, copied HP/RT execution-route bindings, events,
execution traces, and one stable Graph lifetime anchor. It owns graph topology
and session-scoped state, not native platform state. The process
`ExecutionService` owns the fixed `DeviceExecutorRegistry` and its native
device, command queue, invocation allocator, and pipeline cache.
`GraphRuntime` is not the owner of compute dependency planning, policy
contexts, or physical route workers.

**`GraphModel`**
The in-memory graph state: nodes, topology adjacency, parameters, outputs,
cache metadata, timing data, and graph-scoped runtime metadata.

## Graph State and Persistence

**Graph-state operation**
An operation that reads or mutates visible graph state without becoming a
compute task, such as graph document loading, cache commands, inspection, or
ROI projection.

**`GraphStateExecutor`**
The per-graph exclusive access mechanism used by current graph-state operations
and visible compute capture/commit transactions. Its graph-state mode owns one
worker and at most 64 waiting callbacks, excluding the at-most-one active
callback. The private compute-request instance instead charges at most 64 total
queued, running, or parked one-shot/ticket admissions; its active worker is not
an uncharged sixty-fifth unit. A reserved continuation owns one persistent FIFO
node from reservation through retirement, and wake/worker-tail handoff never
self-submits or waits for capacity. Full-capacity admission blocks; close stops
external admission/wake, drains prior one-shot and reserved-ticket work, and
joins the worker. Concurrent closers wait for the durable completion generation
they joined. The executor's generic failure recovery is not a Graph-close
reopen path: lifecycle close is monotonic and never reconstructs either lane
after its marker. The executor remains separate from ready dispatch; its
worker is uncharged infrastructure, not a Run execution grant.

**Graph document**
The persisted representation used to create or update graph state. YAML is the
current concrete format; the term graph document describes the behavior
without treating a serialization library as graph state.

**Graph-document save completion**
The current observation that graph serialization opened, wrote, flushed, and
closed its destination without a reported error. It is separate from a
`ComputeRun` and currently promises no expected-version transaction, atomic
replacement, directory synchronization, or crash-durability receipt.

**Disk-cache artifact**
Discardable acceleration state owned by one safe configured leaf below a
numeric node cache directory. The current entry has four controlled siblings:
an optional image-codec inspection projection, optional detached parameter
metadata, the canonical named-Value archive that is the sole replay authority,
and a versioned manifest written last. The manifest binds archive/metadata
facts to one random writer generation, and replay publishes all Values or none.
This process-serialized, manifest-last mechanism is neither an atomic
filesystem transaction nor crash-durable user output. Current disk persistence
is POSIX-only. On Windows every nonempty-root GraphCache disk request fails
with a typed platform error before side effects; empty-root/no-save no-disk
semantics and pure memory/statistics APIs remain available. Native Windows
GraphCache persistence is a future target.

**`OutputStore` publication**
The current daemon observation that a protected process-scoped canonical
named-Value archive passed identity checks and was published by no-replace
rename after file synchronization. Its index is in memory and retention is
lease/TTL based; no directory synchronization or crash-recoverable index is
claimed.

**Daemon compute-job terminal**
The process-local job-registry state reached after queued/running work either
fails or completes Host compute and, for nonempty Values results, `OutputStore`
publication. It is not a durable acknowledgement and is lost with the process.

**Source-private single-tenant durable named-Value Job vertical (current Issue #99/#100/#105/#131 subset)**
The current `src/lib/server/` vertical binds one canonical nonempty named-Value
archive to stable `ArtifactId` and `OutputCommitId` identities, publishes a
canonical manifest last, and returns a crash-durable receipt only after exact
archive/manifest validation and the complete artifact-directory-to-root barrier chain. Manifest
publication makes both aliases internally recognizable, but an alias whose
barriers are not yet confirmed cannot return an artifact or crash-durable
receipt: `ArtifactId` lookup, `OutputCommitId` lookup, same-commit retry, and
Job reconciliation must revalidate the exact occurrence and replay the full
barrier chain first. One source-private WorkerManager also owns one freshly
execed worker process, exact assignment lease, bounded private protocol,
heartbeat/runtime deadlines, cancellation escalation, and exact reaping per
attempt. See the
[Single-Tenant Job Vertical](Single-Tenant-Job-Vertical.md).

This is a narrow source-private local single-tenant named-Value artifact-set
output and trusted-worker-process subset. It is not the daemon `OutputStore`,
a general `Value`/checkpoint `OutputStore` or bulk data plane, multi-tenant
authorization, a separately deployed WorkerManager, syscall/device isolation,
or an untrusted-plugin security domain.

Product WorkerManager construction requires waitable `SIGCHLD` semantics and
rejects `SIG_IGN` or `SA_NOCLDWAIT`. It revalidates that policy immediately
before `fork`; later auto-reaping, a competing reaper, or any non-`EINTR`
exact-`waitpid` error including `ECHILD` fail-stops before completion
publication or ownership-record deletion. Forced cancellation means an exact
`WIFSIGNALED` status matching an owned delivered `SIGTERM`/`SIGKILL`; successful
`kill()` delivery to a zero-exit zombie is not signal-death evidence.

Exact reaping is not the end of manager ownership: the required typed terminal
fact must also reach the control-plane callback. Each actual first `Report`,
`Failure`, and `ForcedCancellation` fact is built inside a local no-throw
boundary that includes fault injection and identity, message, and report
retention. If that construction raises, including `std::bad_alloc`, WorkerManager cannot
reclassify it through an outer catch: it writes a fixed allocation-free
fail-stop diagnostic and aborts before callback, completed-record marking, or
ordinary record deletion. A callback exception takes the same fail-stop and is
never retried or replaced with a fabricated ordinary completion. Durable Job
and quota truth therefore converge only through restart reconciliation after
this fail-stop.

**General durable output commit (accepted target beyond the Issue #99/#100 subset)**
The broader future user-output transaction is identified by a stable
`OutputCommitId` and completed only after full payload/metadata validation and
file synchronization, canonical manifest staging/validation/file
synchronization, atomic no-replace manifest-last publication, and any requested
leaf-to-durability-root directory barriers. Its typed receipt distinguishes
atomic-visible from crash-durable achievement, recovers ambiguous retries
idempotently, and supports at-least-once delivery; it does not claim
exactly-once delivery. See
[ADR 0009](../adr/0009-compute-io-durability-and-completion-semantics.md).

**Per-graph exclusive access**
The current behavior in which visible Graph capture, mutation, commit
validation, and publication are serialized by one graph-state lane. Long-running
compute operates on a request-owned snapshot outside that lane, while a separate
compute-request lane serializes same-Graph compute and execution-route access.

**`GraphInstanceId`**
The private strong, nonzero, process-lifetime identity of one live Graph
instance. Compute snapshots copy it. Reopening the same user-visible session
label creates a different identity, preventing label-reuse ABA at commit.

**`GraphRevision`**
The private checked nonzero revision of compute-correctness state within one
Graph instance. Scoped structural, document, cache, dirty, and lifecycle
mutations advance it; compute snapshots and successful compute publication
preserve it. Exact identity and revision equality is the current commit
compatibility rule. Product compute additionally requires the Run-captured
complete supersession identity to remain exactly current. Topology generation
remains a separate planning cache key.

**`SupersessionKey` / `SupersessionGeneration` / `SupersessionIdentity`**
The private latest-wins identity inside one live Graph. The key is target node
plus canonical request intent: missing intent and explicit HP are the same
lineage, while realtime is distinct. A checked nonzero graph-wide allocator
gives every prepared candidate a strictly increasing generation and never
wraps or reuses a value. The complete identity contains that key and generation
plus an optional source-private `AcceptedBoundaryCoordinate`. Allocation is
preparatory; graph-state publication assigns the complete current identity. For
two coordinate-bound identities, the accepted coordinate orders replacement
and may authorize a numerically lower generation; mixed and unbound identities
remain generation-ordered. Exact currentness requires equality of generation
and optional coordinate, not numeric-maximum generation. Each admitted key owns
at most one reserved compute-lane ticket, one active/draining candidate, and
one latest pending mailbox value.

**`GraphLifetimeAnchor` / graph lifetime lease**
The stable per-Graph lifetime root registered only after a complete
`GraphRuntime` can publish. A candidate retains a lease from its first
lifecycle check through bundle installation or rollback; an installed Run
retains its lease until commit/discard, resource settlement, and registry
unregistration. The anchor also retains the preallocated monotonic close
coordinator through lane and runtime retirement. It owns no `GraphModel`,
worker, route, policy, or resource authority.

## Compute Planning and Execution

**`ComputeIntent`**
The semantic quality/update intent of a request. `GlobalHighPrecision` and
`RealTimeUpdate` select planning and operation semantics and one copied
per-Graph private execution-route binding. The value is not passed to a policy
as ordering authority and does not define a thread pool, task priority, QoS, deadline,
fairness, cancellation mode, or commit policy.

**`ComputeService`**
The internal compute facade. It coordinates request validation, planning,
cache policy, dirty work selection, operation resolution, dispatch, metrics,
and staged output commit through narrower collaborators.

**`ComputeRun`**
The current private, request-owned execution record for one non-realtime HP
domain or one realtime HP/RT child domain. Its immutable descriptor contains
an opaque non-reused Run id, session label, strong Graph instance identity,
authoritative `GraphRevision`, target, single-domain intent, matching
full/interactive quality, explicit QoS, and immutable request supersession
identity. Its shared control owns monotonic
phase, one exact terminal/commit arbiter, the stable first cancellation reason,
and the full submission plan/temporary results or dirty HP staging required by
its path. The Run mints a private weak-lifetime cancellation source; ordinary
`ComputeRunLease` values can only observe explicit cancellation or an expired
deadline and retain cleanup/commit-contender lifetime. Full, dirty, and
preflight tasks execute owned callbacks through the Host-owned
`ExecutionService` and a closed private route, and publish failure only through
a matching `(RunId, RunLocalTaskId)`.

**Operation return**
The observation that one operation provider callback returned. A callback may
return a pending `Value`; therefore operation return does not imply value
readiness, Run terminal state, result availability, or persistence.

**Value ready**
The observation that a `Value` producer published terminal readiness and that
dependent execution may consume the value. It is later than operation return
for pending producers and earlier than Run success when commit is still
required.

**Run terminal**
The outcome published by the `ComputeRun` terminal arbiter. `Succeeded` means
validated Graph/RT publication, or the admitted valid no-op path; it does not
include cache save, Graph-document save, daemon terminal state, result
delivery, or durable output commit.

**Result available**
The observation that a Host/daemon result can be returned or fetched after
public translation and any transport-owned publication. It is not itself a
durability guarantee.

**`RunGroup`**
The current private request owner for one realtime HP/RT pair. It captures one
realtime supersession identity, owns distinct HP Full and RT Interactive child
Runs plus their observation leases, shares request-wide cancellation and the
monotonic RT-first sibling gate, and deterministically aggregates resource
failure, other failure, group/child cancellation, or success. RT cancellation
or failure before proxy commit denies HP; child-only HP cancellation does not
cancel RT; an already committed RT proxy is not rolled back by later HP or
generation failure. It owns no child plan/dispatcher, worker, Graph state,
resource mint, lifecycle registry, or public cancellation control.

**`RunLifecycleRegistry`**
The process-owned admission and settlement authority inside one
`ExecutionService`. Its single lifecycle fence indexes `Open`/`Closing` Graph
rows, pending candidates, standalone Runs, realtime bundles, finalization, and
one service `Accepting`/`Stopping` generation. It atomically installs a
standalone Run or both realtime children, drives Graph-close/process-shutdown
cancellation, removes a Run only after exact quiescence/resource settlement,
and removes an empty Graph row before lane destruction. It owns no plan,
dispatcher, staged output, graph state, worker, policy decision, or resource
mint.

**`ComputeRunQos`**
The private immutable scheduling inputs captured by a Run: an explicit
`Interactive` or `Throughput` service class, an optional absolute monotonic
deadline, a positive weight, and an optional positive maximum-parallelism
descriptor. The current service applies class, deadline, and weight to policy
ordering and headroom admission. Maximum parallelism caps both the Run root's
CPU/retained/scratch callback-concurrency estimate and the number of its
simultaneously in-flight callbacks; it does not resize the fixed worker pool.
Ready-entry and ready-byte admission still covers every logical task. A
deadline orders interactive work and, when an existing cooperative boundary
observes the injected monotonic clock at or after that value, proposes
`DeadlineExceeded` through the Run's terminal arbiter. This expires the Run
cooperatively without a timer thread or wall clock. Current Kernel requests
use throughput, and none of these values is inferred from intent or output
quality.

**`FullTaskGraph`**
The complete node/tile task shape for one graph generation, compute intent, and
task-shape configuration. Request target, cache state, and dirty state do not
create this shape.

**`ComputePlan` / `ComputeTaskGraph`**
The request-scoped static plan produced by pruning a full task graph to a target
and dependency cone. It remains immutable while execution-visible tasks derived
from it may execute.

**`DirtyRegionSnapshot`**
Graph-scoped Region and lifecycle state that records dirty sources,
authoritative affected Regions, derived image tiles, monolithic work, and edge
mappings. It is not a compute task graph, policy snapshot, ready store, or
execution route.

**`DirtyUpdateWorkSet`**
The active task subset selected from an existing request plan by a dirty
snapshot. Selection may activate or clip planned tasks but does not create new
node or tile task shapes.

**`DirtyControlLane`**
The serialized path that applies node-originated dirty lifecycle updates and
refreshes graph-scoped dirty state. It does not own compute tasks.

**`ComputeTaskDispatcher`**
The execution orchestrator that owns dependency counters, ready release,
temporary-result indexing semantics, and completion aggregation. For current
full HP work, the request `ComputeRun` owns the `TaskSubmissionPlan` storage and
temporary result slots while the dispatcher owns their dependency transitions
and final result commit. Dirty executors reuse its source-first submission
helper and own their staged commit through dirty write buffers. Built-in CPU
full, dirty, and preflight paths push lease-backed owned submissions through the
common ExecutionService boundary.

**`ReadyTaskSubmission`**
A move-only service submission whose compute dependencies are already
satisfied. It owns immutable Run/task identity, a matching lease, an
executable, a priority hint, and a trusted Host resource declaration.
The Host-owned ready store, policy frontier, reserved-start transaction, and
private route may observe only their respective bounded projections; none
receives `GraphModel`, a task graph, or dirty-propagation ownership.

## Policy, Execution, Cache, and Data

**Policy**
A built-in or pure-C ABI v1 selector that receives one immutable bounded
candidate snapshot and returns one candidate id or abstains. A policy owns no
worker, queue, ready entry, resource grant, Run, Graph, executor, completion
route, logger, or lifecycle authority. Its borrowed snapshot is valid only for
the duration of the callback.

**`PolicyClass` / policy binding**
`Interactive` and `Throughput` are the two process service classes. One
process-scoped binding per class owns a built-in or DSO context, a nonzero
generation, an optional immutable first fault, and the DSO leases needed by
metadata/context/invocations. Same-name replacement creates a new context and
generation; both classes remain separate even when they name the same type.

**`PolicyRegistry`**
The process registry of built-in and loaded pure-C policy types. A DSO is
validated and published as one all-or-nothing transaction; unloading removes
visibility while active metadata, bindings, contexts, and invocations retain
independent DSO leases. It is not an execution-plugin registry.

**Host-authored frontier**
The trusted candidate subset selected before any policy callback. Host state
fixes the service class, startability, cancellation/route compatibility,
eight-dispatch aging frontier, earliest finite Interactive deadline, minimum
Graph/Run projected service scores, saturation escape, and stable enqueue
order. A policy may choose only from the immutable original snapshot and cannot
widen that frontier or mint work.

**Execution worker request**
The process configuration value for the fixed Host CPU pool. Zero means bounded
automatic resolution and one through eight are exact. Once configured, zero or
the equal positive value preserves the pool; a conflicting positive value is
invalid. It is not a Run grant, a policy input, or a count of callbacks
currently executing.

**Private execution route**
One of the closed Host implementation ids `cpu`, `serial_debug`, or
`gpu_pipeline`. `GraphRuntime` stores only copied HP/RT ids and nonzero
generations. The Host-owned `ExecutionService` owns physical workers/queues and
route-specific in-flight state; routes cannot be scanned or loaded as plugins.

**`ResourceVector`**
A complete checked request or snapshot with independent CPU-slot,
retained-memory-byte, scratch-byte, ready-entry, and ready-byte dimensions.
Zero declares no amount in that dimension; it never means an unknown amount
that the ledger may invent.

**`ResourceLedger`**
The private Host/device-authoritative mint exclusively owned by one
`ExecutionService`. It atomically admits complete Host vectors and isolated
per-`DeviceId` memory/scratch plans, mints only bounded child grants or exact
device leases, and releases capacity after the true owner ends. A private
release observer may update non-authoritative companion accounting at the exact
Host root-release point, but it cannot mint or enlarge capacity. Default
candidate limits belong to Host composition, not a static process singleton;
service construction creates accounts only for matching executors in the
frozen registry and never inserts one lazily. The ledger owns no worker, ready
ordering, dependency, lifecycle registry, unregistered-device/I/O/plugin
estimate, residency eviction, or fairness authority.

**`ExecutionLifecycleTelemetry`**
The source-private schema-v1 process lifecycle proof store owned by one
`ExecutionService`. It preallocates 65,536 fixed records, publishes 15 event
kinds and a complete 15-counter post-transition view, and provides
non-destructive atomic-cut pages of 1..4,096 records with explicit cursor gaps
and saturating drop totals. Six trusted physical counter selectors cover ready
entries, entered operation callbacks, live root reservations, live child
grants, policy invocations, and current/displaced bindings; registry-derived
counters come only from `RunLifecycleRegistry`. M1 lifecycle replay treats the
nine registry-derived counters as an exact state-machine projection over
Graph, candidate, bundle, Run, group, and generation identity. The six physical
counters remain independent samples checked for capacity and ownership
reachability, not event-kind deltas; physical retirement publishes its exact
registry cut under the registry lifecycle fence. `ServiceStopped` is the final
record and carries zero in all 15 fields. Records contain copied scalar
identities and grant no lifecycle, queue, callback, plugin, Graph, or Run
authority. No Host, CLI, or IPC method exposes this store.

**Bounded ready store**
The `ExecutionService`-owned policy-aware store whose aggregate entry and
accounted-byte counts cannot exceed immutable ledger limits. It bills one
dispatch as `work_units + ceil(complete_ready_grant_bytes / 4096)`, accumulates
raw Graph service independently per selected class and weight-normalized Run
service within each Run's immutable class, honors interactive deadline
ordering, and first selects a service class using fixed three-to-one
Interactive/Throughput arbitration. Within that selected class, ready entries
age after eight successful dispatches before ordinary policy comparison;
aging never changes the selected class. Initial and dependency-released
submissions cross the same boundary, and Run rows persist across temporary
emptiness. Removing an entry releases its ready grant only after execution
authority is acquired or the entry is purged.

**Scheduler-selectable candidate**
A worker-local ready lane head that passes current Run lifecycle,
cancellation, operation-gate, physical-route, and cycle-local grant-block
visibility. It participates in class choice, the policy frontier, and the
three-to-one Interactive burst state. Remaining execution child-grant capacity
is deliberately not a selector predicate: exhaustion is classified only by
reserved start, which marks the exact entry for that worker without charging
dispatch or fairness state.

**Evidence-startable class fact**
An observation-only per-class Boolean sampled for a service start from the
scheduler-visible ready/lifecycle/operation/route predicates plus sufficient
live child-grant capacity. It is published only with a successfully committed
start and controls M1 applicability, not candidate selection or burst
accounting. It mints no grant and cannot replace `try_grant()`.

**Resource reservation and grant**
A reservation is the move-only RAII owner of one atomically admitted root
vector; a grant is a move-only, non-forgeable child authority minted within
that vector. A Run root remains committed until every queued/executing child
grant has ended. Ready-entry/byte grants cover queued submissions; reserved
start exchanges the exact selected ready grant for CPU/retained/scratch
execution grants. A Throughput Run's non-authoritative class-quota charge has
the same root lifetime; Interactive roots do not debit that quota.

**Reserved start**
The Host-only transaction between policy selection and executor entry. A
private `SelectionPin` identifies the exact ready entry/version; a
`StartTransaction` stages resource grants, rechecks current Run/cancellation/
route/fairness state, and then commits an allocation-free no-throw removal,
service-accounting update, and callback transfer. A rejected or throwing
pre-commit path changes no ready/fairness state and releases staged grants once.

**`ExecutionTaskRuntime`**
The private push-only task/completion adapter used after a reserved start. It
accepts initial and newly released ready work, publishes route worker/epoch
attribution, and settles completion or the first exception. It does not pull
from a plan, derive tasks, inspect Graph topology, rank candidates, mint
resources, or commit Graph state. It is not an installed extension ABI.

**`ExecutionTaskPriority`**
The current independent `Normal` or `High` ready hint. It is orthogonal to
`ComputeIntent`: HP and RT dirty source batches both use `High`, while their
downstream groups use `Normal`. In the service policy store it is not an
absolute priority: within the service class already selected by inter-class
arbitration, aging can select an older normal-hint entry through a continuing
high-hint stream.

**Execution epoch**
A private nonzero route/runtime batch identity used for attribution and stale
completion isolation after Host admission. It is not a policy generation,
binding generation, dirty generation, Graph revision, Run identity, deadline,
or cooperative cancellation token.

**HP cache**
`cached_output_high_precision`, the only authoritative reusable in-memory
image result stored on a graph node.

**`RealtimeProxyGraph`**
Runtime-owned transient RT output state. It is not authoritative HP cache and
is not persisted as reusable graph cache.

**`AllocationIdentity`**
An opaque process-local identity for one CPU allocation control block.
BufferHandle subranges over that allocation compare equal by allocation
identity even when their ranges differ. It is not a Value revision, content
digest, persistent cache key, or task identity. `valid()` reports only that the
token is nonzero and was issued; it is not a liveness query, and a copied token
remains nonzero after the final allocation owner is destroyed. The shared
operation runtime mints tokens through one process-wide authority used by the
Host and Value-using DSOs.

**`BufferHandle`**
A checked immutable nonempty byte range over one allocation identity. It does
not expose a raw pointer. Consumers reach bytes through retaining `ReadLease`
objects; construction-time writes are controlled by `ValueBuilder`.

**`ReadLease` / `WriteLease`**
`ReadLease` is a copyable retaining capability for immutable byte access.
`WriteLease` is the unique move-only builder-scoped capability for mutable byte
access before seal. A live write lease blocks seal; sealed Values never issue
one.

**`ValueRevisionId`**
An opaque process-local identity minted whenever `ValueBuilder` seals a new
Value publication. Ordinary Value/cache copies preserve it; dirty mutation,
replacement, and disk reload mint a new one. It is not `GraphRevision`, an
allocation identity, a persistent digest/artifact id, or a task/cache key.

**`ImageBounds` / data window / display window**
`ImageBounds` is a signed nonempty half-open ordinary-image coordinate window.
Every built-in `ImageFacet` has one immutable data window whose x/y spans
exactly match its explicit tensor axes. An optional display window is separate
presentation metadata. Neither is a `RegionSet`: Region remains dynamic work
or validity. Bounds metadata remains readable when payload readiness is
Pending, Failed, or ProducerCancelled.

**`ChannelSchema` / `ChannelId` / `ChannelGroupId`**
The bounded stable semantic identities of ordinary image channels and groups.
Channel vector order matches the channel axis; group membership uses stable
IDs. Channel and group names are diagnostics only and never select roles or
enter semantic identity.

**`SampleDomainFacet` / `ColorFacet`**
Independent versioned ordinary-image interpretations. Sample encoding/domain
declares normalized, legal, or code-value intervals and optional stable-ID
per-channel overrides; it does not change storage representability or
quantization. Color binds one stable channel group to explicit transfer
function and primaries; scene linearity is a color fact.

**Image statistics query / result / cache key**
Bounded derived records for observed min/max or histogram requests. Their key
includes Value revision, optional content digest, RegionSet, stable channel or
group selection, algorithm, and version. Results never become Value,
ImageFacet, descriptor/content identity, or formal cache validity.

**Ordinary dense image `Value`**
The sole current ordinary-image payload contract: one immutable DenseTensor
`Value` with complete `ImageFacet`, explicit Layout and bindings, and a
`ReadyFence`. Host, IPC, worker, durable, codec, CLI, operation ABI, and cache
boundaries preserve that Value metadata or its portable artifact form. It is
not a provider-defined variable-sample Deep image, vector-scene model, or
general rank-free Tensor interpretation. DI-4 removed the former two-
dimensional compatibility image object rather than retaining a dual surface.

**`RegionDomainKey` / `RegionSet`**
`RegionDomainKey` is a permanent 128-bit logical coordinate-domain identity.
`RegionSet` is the immutable normalized logical work/validity value. The V-4
MVP supports canonical Empty, Whole, one bounded nonempty conjunction, signed
half-open ImageRect, rank-general unsigned half-open TensorSlice, explicit
complexity budgets, typed algebra outcomes, and containment. It is not
physical tile geometry, a cache owner, a Value revision, or an uncertainty
placeholder.

**`PixelRect` / `PixelSize`**
External-library-neutral two-dimensional integer geometry used by current
Host/IPC inspection, operation ABI v1 adapters, dense-Value processing, and
physical image tile/task records. It is derived from one exact built-in ImageRect where
logical Region authority is required. It cannot represent TensorSlice, Whole,
custom domains, multi-atom clauses, or uncertainty. OpenCV geometry may be
constructed only locally in an OpenCV adapter or provider at the actual
matrix/library call.

**Operation provider**
An implementation source for operation callbacks, propagation contracts, and
metadata. Dependency-neutral core operations are always composed at process
seed. The repository OpenCV CPU provider is a separate optional build module
that owns its algorithms, process initialization, and exception translation.
Both it and pure-C operation ABI v1 DSO generations publish into the same
provider-neutral registry slots, so a DSO can replace an active operation and
unload restores its predecessor. Public operation contracts use Photospider
values.

**Adapter**
A narrow translation at an external library, transport, or product edge. An
adapter converts representations without becoming the owner of graph,
planning, cache, policy, or physical-execution semantics.

## Terms That Must Remain Distinct

- `ComputeIntent` is not `PolicyClass`, execution priority, QoS, or commit
  policy.
- Dirty generation is not execution epoch, policy binding generation, or route
  generation.
- A graph-state operation is not a compute task.
- `DirtyRegionSnapshot` is not `ComputeTaskGraph`.
- A ready task is not a task graph.
- HP cache is not RT proxy state.
- `AllocationIdentity` is not `ValueRevisionId`; neither is a persistent
  content/cache identity.
- An ordinary dense image `Value` is not the provider-defined or rank-general
  [general data model](../roadmap/Kernel-Evolution.md#general-data-and-regions).
- Operation return is not `Value` readiness, and neither is Run terminal
  publication.
- Run success is not cache persistence, Graph-document save, durable output
  commit, daemon terminal state, or result delivery.
- Current `OutputStore` publication is not crash-durable output commit.
- The current single-tenant named-Value artifact vertical is not the general
  `OutputStore`/bulk data plane, network control plane, or untrusted-plugin
  security domain.
- Daemon job terminal state or acknowledgement is not a durable receipt.
- `RegionSet` is not `PixelRect`; the latter is a checked image-edge
  projection and never TensorSlice authority.
- An execution worker request is not a Run reservation or child grant.
- A policy candidate id or decision is not execution authority; only a
  committed reserved-start transaction may enter a private route.
- A policy binding generation is not a route generation, snapshot generation,
  Run id, or supersession generation.
- A `ResourceVector` is not a worker pool, observed allocation total, or a
  license to guess undeclared device/I/O/plugin dimensions.
- A root reservation is not an executing callback; a child grant is not
  transferable outside its ledger-created ownership path.
- Lifecycle telemetry is observation, not admission, cancellation, resource,
  policy, or close authority.
- A policy owns ordering only; a private execution route owns physical entry
  only; a Run owns request correctness and settlement; the Host owns validation
  and resource authority.

## Implementation and Validation Entry Points

- `include/photospider/memory/buffer_handle.hpp`
- `include/photospider/data/value.hpp`
- `include/photospider/host/host.hpp`
- `include/photospider/core/compute_intent.hpp`
- `include/photospider/data/image_view.hpp`
- `include/photospider/data/value_artifact.hpp`
- `include/photospider/policy/policy_plugin_api.h`
- `src/lib/runtime/graph_runtime.hpp`
- `src/lib/graph/graph_model.hpp`
- `src/lib/graph/graph_state_executor.hpp`
- `src/lib/graph/graph_io_service.*`
- `src/lib/graph/graph_cache_service.*`
- `src/lib/compute/dispatch/task_graph_planning.hpp`
- `src/lib/compute/dirty/dirty_region_snapshot.hpp`
- `src/lib/compute/execution/execution_service.hpp`
- `src/lib/compute/execution/run_lifecycle_registry.hpp`
- `src/lib/compute/execution/execution_lifecycle_telemetry.hpp`
- `src/lib/execution/execution_task_runtime.hpp`
- `src/lib/policy/policy_registry.hpp`
- `src/lib/compute/request/compute_request_coordinator.hpp`
- `src/lib/compute/request/compute_supersession.hpp`
- `src/lib/compute/dispatch/run_group.hpp`
- `src/lib/runtime/resource_ledger.hpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_resource_admission.cpp`
- `tests/unit/test_resource_ledger.cpp`
- `tests/unit/test_policy_registry.cpp`
- `tests/unit/test_compute_supersession.cpp`
