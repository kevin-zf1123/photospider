# ADR 0003: Execution Resources Are Process-Owned

## Status

Accepted as a target architecture. Issues #70 and #71 implement the current CPU
execution/resource and scheduling slice: each embedded composition root
explicitly creates and injects one fixed `ExecutionService`; built-in CPU HP
and RT work, including connected-parameter preflight and dirty
source/downstream phases, enters it only as ready, lease-backed submissions.
Independent Runs from multiple Graphs may overlap on that pool, and built-in
CPU intent bindings no longer allocate per-Graph scheduler owners. Serial, GPU,
and plugin routes remain per-Graph schedulers. The service exclusively owns a
Host-authoritative ledger and a policy-aware, entry/byte-bounded ready store;
complete CPU/retained/scratch/ready Run vectors and conservative legacy
scheduler CPU slots share that authority. Private interactive and throughput
strategies order work with work/byte cost, hierarchical Graph/Run accounting,
deadline preference, aging, interactive headroom, and bounded throughput
progress. Issue #72 now keeps strong Graph identity, authoritative revision,
request-owned staging, and revision-safe publication outside the execution
service. Device, I/O, and plugin-specific accounting, `RunGroup`, cancellation,
and generation supersession remain target behavior. ADR 0007 supersedes this
ADR only as the detailed
ownership and lifecycle contract; the high-level process ownership decision
and its historical context remain in force.

## Context

Each current `GraphRuntime` owns a binding for each intent. A binding either
owns a transitional serial, GPU, or plugin scheduler, or selects the injected
built-in CPU `ExecutionService` without a Graph-owned scheduler. Legacy
scheduler interfaces still combine policy, worker lifecycle, queues, batches,
device routing, completion, and exceptions. The service instead freezes one
CPU worker count before first use, keeps isolated completion/failure/trace
state per Run, and permits independent HP and RT Runs from multiple Graphs to
overlap.

Current software limits legacy worker multiplication with resolved grants and
each Host ledger's default 32-slot CPU dimension. Fixed service workers are
infrastructure, while active Runs and legacy scheduler owners compete for the
same CPU authority. Retained Host memory, scratch, ready entries, and ready
bytes are admitted too. The current service enforces the Issue #71 CPU
fairness and headroom contract. Issue #72 exact revision validation remains a
Kernel/graph-state commit concern outside the service; cancellation,
supersession, and new device/I/O/plugin dimensions remain outside this slice.

Moving those schedulers to a global object without introducing a stable Run
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

`SchedulerPolicy` is an internal strategy seam of `ExecutionService`.
`InteractiveSchedulerPolicy` and `ThroughputSchedulerPolicy` rank already
admitted ready work. They do not own threads, the physical ready store,
resource tokens, budget, Graph state, or native device handles. The service
owns policy state and the store, while a host-owned `ResourceLedger` validates
all reservations and releases them exactly once.

Physical execution is divided into resource executors:

- a process CPU executor;
- one executor per physical GPU/device, with native queues and fences;
- bounded compute I/O executors;
- a plugin invocation adapter backed by a separate
  `PluginRuntimeSupervisor` for process, IPC, security, and failure isolation.

The current worker-owning scheduler plugin ABI is replaced as a complete
breaking migration after built-in policies prove the new seam.

## Consequences

- Thread and device-queue counts are controlled by process configuration rather
  than graph count.
- Interactive and throughput Runs can share resources under explicit fairness,
  deadline, and headroom policy.
- Graph revision, cancellation, and stale-result rejection become Run-level
  correctness rules rather than scheduler epoch hints.
- GPU completion and I/O continuation may outlive a caller stack, so task
  handles require stable Run leases instead of borrowed executor pointers.
- The service must remain a deep module; Graph planning, persistence, cache
  authority, and commit semantics stay outside it.
- Plugin process supervision remains separate so execution-resource ownership
  does not become a monolithic security subsystem.

## Relationship to Current Documentation

ADR 0001 remains fully in force. Issues #69 through #71 supersede the built-in
CPU physical ownership and scheduling described by the per-graph scheduler
sections of
`docs/kernel-architecture/Scheduler-Architecture.md`: HP, RT, preflight, and
dirty ready work all execute on the injected fixed service. Built-in CPU
bindings are ownerless at `GraphRuntime`; serial, GPU, and plugin routes retain
legacy scheduler owners. The ready-task-only boundary remains fully in force.
Issue #72 additionally keeps request-owned staged Graph/proxy state, exact
identity/revision validation, and visible publication on the
compute/graph-state side of that boundary.

The current contract accepts worker requests from zero through eight, resolves
zero to a nonzero grant capped at eight, and freezes the service count once.
Every Host ledger has immutable composition limits. Run admission commits one
complete vector before queue publication; initial and dependent submissions
enter the same policy-aware bounded store and retain the same Run fairness row
across temporary emptiness. Ready cost is `work_units + ceil(bytes / 4096)`;
Graphs are charged raw cost independently in each selected service class and
Runs are charged `ceil(cost / weight)` in their immutable class. An earlier
interactive deadline wins within its policy class, a ready item ages after
eight successful dispatches, and at most three interactive dispatches may
precede required throughput progress while throughput remains ready.
Configured interactive headroom caps only active built-in Throughput root
reservations at the general ceiling. Interactive and transitional Issue #70
legacy roots do not debit that class quota, while the ledger remains final
authority for all shared physical capacity. Throughput check, reservation, and
class charge are atomic, and the charge remains until exact root release after
all child grants.
Graph load reserves only legacy HP/RT owners, and legacy replacement still
requires transient candidate capacity while its old owner remains live;
built-in CPU load or replacement publishes an ownerless service route and
never resizes the pool. The former worker-only budget is completely removed
without a wrapper, alias, or second authority.

## Relationship to ADR 0007

[ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.md)
preserves this decision's process-owned execution direction and ADR 0001
boundary while superseding the implicit details. It is authoritative for Run
identity and leases, monotonic terminal state, completion routing, target
`GraphRuntime` non-ownership, ledger token authority, commit races, graph/process
shutdown scope, and the issue #66–#76 dependency contract.
