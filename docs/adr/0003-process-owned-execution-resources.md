# ADR 0003: Execution Resources Are Process-Owned

## Status

Accepted as a target architecture. Issues #70 through #75 implement the current
execution/resource, policy, and private-route slice: each embedded
composition root explicitly creates and injects one fixed `ExecutionService`;
built-in CPU HP and RT work, including connected-parameter preflight and dirty
source/downstream phases, enters it only as ready, lease-backed submissions.
Independent Runs from multiple Graphs may overlap on that pool. `GraphRuntime`
stores only copied HP/RT route ids and nonzero generations; it owns no physical
worker, queue, policy context, or plugin DSO lifetime. The service exclusively
owns a Host-authoritative ledger and an entry/byte-bounded ready store;
complete CPU/retained/scratch/ready Run vectors share that authority. One
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
routes, including one fixed CPU pool and one private Metal lane. The final
lifecycle registry/graph-close/process-shutdown/telemetry
contract (#76) and public Host/CLI/IPC cancellation controls remain future
behavior. ADR 0007 supersedes this ADR only as the detailed
ownership and lifecycle contract; the high-level process ownership decision
and its historical context remain in force.

## Context

Each current `GraphRuntime` stores copied route bindings for HP and RT intent.
The route vocabulary is closed to `cpu`, `serial_debug`, and
`gpu_pipeline`; their physical workers, queues, device routing, completion, and
exceptions remain private to Host execution modules. Policy binding is
process/service state and never Graph state. The service freezes one CPU worker
count from composition-root configuration, owns one fixed Metal worker lane,
and keeps isolated
completion/failure/trace state per Run, and permits independent HP and RT Runs
from multiple Graphs to overlap.

The canonical device inventory is route aware. `cpu` and `serial_debug` expose
CPU only. `gpu_pipeline` exposes Metal then CPU when the Host reports Metal,
otherwise CPU only. Full, dirty HP/RT, and connected-preflight planning freeze
the selected implementation and device before admission. CPU and Metal work
use distinct fixed lanes but the same ready store, Run parallelism ceiling,
ledger grants, cancellation, completion, exception, reuse, and drainage state.

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
current Issue #74 behavior; final lifecycle-driven graph-close/process-shutdown
cancellation remains Issue #76 work. Issue #75 separates policy comparison
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
state and the store, while a Host-owned `ResourceLedger` validates all
reservations and releases them exactly once. `PolicyRegistry` owns immutable
built-in and DSO policy type records; DSO callbacks use the self-contained C11
policy ABI v1 and receive only scalar candidate snapshots.

Physical execution is divided into resource executors:

- a process CPU executor;
- one executor per physical GPU/device, with native queues and fences;
- bounded compute I/O executors;
- a plugin invocation adapter backed by a separate
  `PluginRuntimeSupervisor` for process, IPC, security, and failure isolation.

The current #75 slice realizes the CPU executor and one service-owned Metal
lane. It does not expose a device-executor API or add a second device-capacity
ledger; later resource executors remain target architecture.

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

ADR 0001 remains fully in force. Issues #69 through #75 supersede the per-Graph
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
