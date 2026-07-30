# ADR 0003: Execution Resources Are Process-Owned

## Status

Accepted. Issues #70 through #76 implement the current
execution/resource, policy, and private-route slice: each embedded
composition root explicitly creates and injects one fixed `ExecutionService`;
built-in CPU HP and RT work, including connected-parameter preflight and dirty
source/downstream phases, enters it only as ready, lease-backed submissions.
Independent Runs from multiple Graphs may overlap on that pool. `GraphRuntime`
stores only copied HP/RT route ids and nonzero generations; it owns no physical
worker, queue, policy context, or plugin DSO lifetime. The service exclusively
owns a Host-and-per-device authoritative ledger and an entry/byte-bounded ready
store; complete CPU/retained/scratch/ready Run vectors share the Host vector.
One
Interactive and one Throughput policy binding order work behind Host-authored
class, frontier, fairness, and fallback rules. Issue #72 keeps strong Graph
identity, authoritative revision, request-owned staging, and revision-safe
publication outside the execution service. Issue #73 gives each current Run a
private weak-lifetime cancellation source, read-only lease/deadline
observation, one terminal/commit arbiter, exact-Run queued purge, running
drainage, dependent rejection, and RT-denies-HP cancellation. Issue #74 adds
request-level realtime `RunGroup`, checked per-Graph latest-wins generations,
bounded ticket-backed coalescing on the existing compute-lane worker, and
current-generation commit authority. Issue #75 removes the worker-owning
scheduler SDK/ABI and adds pure-C policy ABI v1, atomic binding replacement,
generation-local sticky faults, reserved start, and closed private execution
routes, including one fixed CPU pool and one private Metal lane. Issue #76
implements the lifecycle registry, monotonic Graph close, explicit process
execution shutdown, exact settlement, and source-private telemetry. Issue #84
removes per-Graph native Metal ownership and installs a fixed
`DeviceExecutorRegistry` in `ExecutionService`; its Metal executor owns one
device, command queue, invocation allocator, and persistent pipeline cache and
enters the selected operation only after reserved start. Issue #85 adds
explicit revision-preserving CPU/Metal transfer, exact completion identity,
one shared process-owned `ResidencyManager`, and Run-bound pending-Value
continuations without creating another ready store or capacity authority.
Issue #86 adds isolated configured non-CPU `DeviceId` memory/scratch accounts,
native plan/actual reconciliation, and owner-bound persistent/completion
leases to that sole service ledger.
Public Host/CLI/IPC cancellation controls remain future behavior. ADR 0007 supersedes this ADR only
as the detailed
ownership and lifecycle contract; the high-level process ownership decision
and its historical context remain in force.

## Context

Each current `GraphRuntime` stores copied route bindings for HP and RT intent.
The route vocabulary is closed to `cpu`, `serial_debug`, and
`gpu_pipeline`; their physical workers, queues, device routing, completion, and
exceptions remain private to Host execution modules. Policy binding is
process/service state and never Graph state. The service freezes one CPU worker
count from composition-root configuration, owns one fixed Metal worker lane
and one immutable device-executor registry with a shared residency manager, and keeps isolated
completion/failure/trace state per Run, and permits independent HP and RT Runs
from multiple Graphs to overlap.

The canonical device inventory is route aware. `cpu` and `serial_debug` expose
CPU only. `gpu_pipeline` exposes Metal then CPU when the fixed registry contains
a usable Metal executor, otherwise CPU only. Full, dirty HP/RT, and
connected-preflight planning freeze the selected implementation and device
before admission. CPU and Metal work use distinct fixed lanes but the same
ready store, Run parallelism ceiling, ledger grants, cancellation, completion,
exception, reuse, and drainage state. After reserved start, non-CPU work enters
the matching registry executor synchronously; no Graph or policy object
receives a native handle.

Current software uses each Host ledger's default 32-slot CPU dimension for Run
execution grants. Fixed service workers and route machinery are
infrastructure. Retained Host memory, scratch, ready entries, and ready bytes
are admitted too. The current service enforces the Issue #71 CPU
fairness and headroom contract. At the Issue #72 delivery snapshot, exact
revision validation remained a Kernel/graph-state commit concern outside the
service, while cancellation and supersession remained outside that historical
slice. Current software now implements Issue #73 cooperative cancellation as
Run-owned terminal correctness: the service observes and purges/drains only the
matching Run, while the graph-state transaction arbitrates cancellation against
commit. Latest-wins supersession and request-level realtime grouping are now
current Issue #74 behavior; lifecycle-driven Graph-close/process-shutdown
cancellation is current Issue #76 behavior. Issue #75 separates policy comparison
from execution ownership: the Host builds the frontier and validates decisions,
while a pure-C callback can only choose one immutable candidate or abstain.

Moving physical executors to a global object without introducing a stable Run
lifetime and host-owned resource accounting would only relocate the problem.

## Decision

One explicit process-owned `ExecutionService` owns physical CPU workers, device
executors, compute I/O workers, ready-store capacity, admission, and resource
accounting. It is created at the product composition root and injected; it is
not a static singleton.

`ComputeRun` is the request-owned unit of compute identity, cancellation,
temporary output, terminal state, graph revision, supersession, resource
reservation, and commit policy.

`ComputeTaskDispatcher` continues to own task dependencies and ready detection.
Only `ReadyTaskSubmission` values enter `ExecutionService`, preserving ADR
0001.

Policy binding is an internal comparison seam of `ExecutionService`. One
Interactive and one Throughput binding rank already admitted ready work through
the same Host-authored frontier and validation path. They do not own threads,
the physical ready store, resource tokens, budget, Graph state, native device
handles, completion routes, or lifecycle authority. The service owns binding
state and the store, while a service-owned `ResourceLedger` validates all
reservations and releases them exactly once. `PolicyRegistry` owns immutable
built-in and DSO policy type records; DSO callbacks use the self-contained C11
policy ABI v1 and receive only scalar candidate snapshots.

Physical execution is divided into resource executors:

- a process CPU executor;
- one executor per physical GPU/device, with native queues and fences;
- bounded compute I/O executors;
- a plugin invocation adapter backed by a separate
  `PluginRuntimeSupervisor` for process, IPC, security, and failure isolation.

The current #84 through #86 slices realize the CPU executor, one service-owned
Metal lane, a source-private fixed device-executor registry, explicit
CPU/Metal transfer, exact process-owned residency, and authoritative
per-`DeviceId` memory/scratch accounting. In the enabled
repository Metal-plugin profile, the Apple entry owns and reuses its native
device/queue and validated pipeline cache, while each entry receives an
invocation-scoped native allocator. Before native allocation, Perlin and
CPU-to-Metal upload atomically reserve complete device plans derived from
Metal heap size/alignment queries. Native `allocatedSize` facts reconcile the
plans before command commit: unused bytes return immediately, persistent
memory leases move into the native `Value` owner, and scratch leases move into
the exact command-completion owner. Perlin publishes a pending native Value,
encodes texture-to-buffer readback, and returns without a command-buffer wait.
Completion freshness, applicable producer Ready publication, destination Ready
publication, and resident insertion are one manager-locked transaction.
Kernel first pretracks the lineage without advancing it before fallible
coordinator submission. An accepted current publication then performs a
no-allocation manager advance while the coordinator still excludes currentness
observation; rejected and born-stale candidates do not. This prevents a late
older Run start from regressing the manager generation.
Pending-Value continuation reuses the existing Run and ready store. This adds
no public device-executor API, no Graph/cache authority, and no second
device-capacity ledger. The service-owned `ResourceLedger` remains the sole
authority: Host dimensions retain their meanings while each configured
non-CPU `DeviceId` has isolated immutable memory/scratch limits and copied
limits/reserved/available diagnostics.

The worker-owning scheduler plugin ABI, SDK target, `IScheduler` hierarchy, and
per-Graph physical owners have been removed as a complete breaking migration.
No compatibility adapter or forwarding layer remains.

## Consequences

- Thread and device-queue counts are controlled by process configuration rather
  than graph count.
- Interactive and throughput Runs can share resources under explicit fairness,
  deadline, and headroom policy.
- Graph revision, cancellation, and stale-result rejection become Run-level
  correctness rules rather than policy-binding or route-generation hints.
- GPU completion and I/O continuation may outlive a caller stack, so task
  handles require stable Run leases instead of borrowed executor pointers.
- The service must remain a deep module; Graph planning, persistence, cache
  authority, and commit semantics stay outside it.
- Plugin process supervision remains separate so execution-resource ownership
  does not become a monolithic security subsystem.

## Relationship to Current Documentation

ADR 0001 remains fully in force. Issues #69 through #76 supersede the per-Graph
physical ownership and worker-owning scheduler model described by historical
versions of `docs/kernel-architecture/Policy-and-Execution-Architecture.md`:
HP, RT, preflight, and dirty ready work all pass through the injected fixed
service. `GraphRuntime` owns only copied route ids/generations; serial-debug,
shared-CPU, and GPU-pipeline execution stay behind private Host routes. The
ready-task-only boundary remains fully in force.
Issue #72 additionally keeps request-owned staged Graph/proxy state, exact
identity/revision validation, and visible publication on the
compute/graph-state side of that boundary. Issue #73 adds a private request
cancellation coordinator, independent HP/RT child sources, cooperative
monotonic deadline expiry, and Run-owned terminal/commit contention on that
same side. `ExecutionService` registers a read-only cancellation notification,
purges only the matching Run's queued entries, suppresses dependent re-entry,
and waits for non-preemptible running callbacks to drain; it does not become
cancellation authority or visible-commit owner.

Composition-root execution configuration resolves and freezes the service CPU
worker count; the single Metal lane is fixed infrastructure, not a policy-plugin
grant. Every Host ledger has immutable
composition limits. Run admission commits one complete vector before queue
publication; initial and dependent submissions enter the same policy-aware
bounded store and retain the same Run fairness row across temporary emptiness.
Ready cost is `work_units + ceil(bytes / 4096)`; Graphs are charged raw cost
independently in each selected service class and Runs are charged
`ceil(cost / weight)` in their immutable class. The Host chooses a class,
constructs a bounded frontier, permits at most three consecutive Interactive
starts while Throughput remains startable, and validates every built-in or DSO
decision against the original snapshot and current state. A first invalid DSO
decision is sticky for its exact binding generation. Reserved start atomically
removes the exact ready entry, exchanges ready authority for execution grants,
updates fairness/burst state, and transfers callback ownership to a private
route before any executor callback begins.

Configured interactive headroom caps only active Throughput root reservations
at the general ceiling. Interactive Runs do not debit that class quota, while
the ledger remains final authority for all shared physical capacity.
Throughput check, reservation, and class charge are atomic, and the charge
remains until exact root release after all child grants. Cancellation accepted
before the graph-state commit contender publishes no Graph, proxy, or deferred
cache state. Once that contender wins, late cancellation is a no-op;
predicate/persistence failure or visible success resolves the same Run arbiter.
RT cancellation before proxy commit denies and cancels HP, while HP
cancellation cannot roll back an already committed RT proxy.

Graph load and route replacement copy only validated route ids and nonzero
generations and do not reserve or construct a Graph-owned physical owner.
Service-level policy replacement prepares a new context before publication,
publishes one new generation atomically, drains the old generation's active
invocations, and retires its context/DSO lease exactly once. The former
worker-only budget and scheduler SDK are completely removed without a wrapper,
alias, or second authority.

## Relationship to ADR 0007

[ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.md)
preserves this decision's process-owned execution direction and ADR 0001
boundary while superseding the implicit details. It is authoritative for Run
identity and leases, monotonic terminal state, completion routing, target
`GraphRuntime` non-ownership, ledger token authority, commit races, graph/process
shutdown scope, and the issue #66–#76 dependency contract.

## Relationship to ADR 0008

[ADR 0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md)
extends, rather than reverses, this decision. The injected process execution
domain owns the target Schema, Facet, Layout, access, conversion, query,
inference, digest, and execution-provider registries; immutable published
generations and their leases follow the same prepare, publish, retire, and
unload discipline as other process-owned execution resources. A `Value`,
`StorageBinding`, `ReadyFence`, or residency replica does not own workers,
admission, ready queues, policy authority, or ResourceLedger tokens.

ADR 0008 is authoritative for generic-value and provider-generation contracts.
ADR 0007 remains authoritative for Run identity, execution admission,
ready-work release, resource grants, cancellation, commit arbitration, Graph
close, and process shutdown. Implementing generic values must not restore
Graph-owned physical executors or create a second execution-resource authority.

## Relationship to ADR 0009

[ADR 0009](0009-compute-io-durability-and-completion-semantics.md) fixes the
boundary for the I/O continuation anticipated by this decision. A future
process-owned `ComputeIoExecutor` owns bounded cache, asset, and codec
mechanism, with admission limited by operation count and bytes. It does not own
Graph-document transactions, daemon transport, cache authority, output commit
identity, retry/overwrite policy, or durability.

`ComputeRun` success, cache persistence, Graph-document save, daemon result
availability, and durable output commit remain distinct outcomes. Adding I/O
workers must not broaden the process execution service into a persistence
authority or create a second visible-commit owner.
