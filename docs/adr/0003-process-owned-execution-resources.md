# ADR 0003: Execution Resources Are Process-Owned

## Status

Accepted as a target architecture. Issue #69 implements the current CPU
process-execution slice: the embedded composition root explicitly creates and
injects one fixed `ExecutionService`; built-in CPU HP and RT work, including
connected-parameter preflight and dirty source/downstream phases, enters it
only as ready, lease-backed submissions. Independent Runs from multiple Graphs
may overlap on that pool, and built-in CPU intent bindings no longer allocate
per-Graph scheduler owners. Serial, GPU, and plugin routes remain transitional
per-Graph schedulers. The process-wide scheduler-worker budget now counts each
fixed service pool as well as legacy scheduler workers, but remains a
containment step rather than the target resource owner. Final resource
accounting, fairness policy, `RunGroup`, cancellation, and revision-safe commit
remain target behavior. ADR 0007 supersedes this ADR only as the detailed
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

Current software limits transitional worker multiplication with resolved
grants and one conservative 32-slot process budget shared across embedded
Hosts. The budget charges each Kernel's fixed CPU service pool once and charges
legacy scheduler-owned workers per Graph. Neither that budget nor the current
two-priority service expresses final cross-Run fairness, process memory limits,
cancellation, or resource admission beyond worker slots.

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

`SchedulerPolicy` is an internal strategy seam of `ExecutionService`. It ranks
ready work and suggests bounded grants, but does not own threads, queues,
resource tokens, Graph state, or native device handles. A host-owned
`ResourceLedger` validates all grants and releases reservations exactly once.

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

ADR 0001 remains fully in force. Issue #69 supersedes the built-in CPU physical
ownership described by the per-graph scheduler sections of
`docs/kernel-architecture/Scheduler-Architecture.md`: HP, RT, preflight, and
dirty ready work all execute on the injected fixed service. Built-in CPU
bindings are ownerless at `GraphRuntime`; serial, GPU, and plugin routes retain
transitional scheduler owners. The ready-task-only boundary remains fully in
force.

The current containment contract accepts worker requests from zero through
eight, resolves zero to a nonzero grant capped at eight, freezes the service
count once, and reserves at most 32 conservative worker slots across all
embedded Hosts. One pool-lifetime RAII reservation covers each fixed CPU
service. Graph load reserves only legacy HP/RT owners, and legacy replacement
still requires transient candidate headroom; built-in CPU load or replacement
publishes an ownerless service route and never resizes the pool. This prevents
unbounded worker multiplication while legacy workers, queues, epochs, and
policies remain inside their `IScheduler` instances.

`SchedulerWorkerBudget` is therefore neither the current `ExecutionService` nor
the target `ResourceLedger`: it owns no executor, Run identity, cancellation,
fairness, memory/device/I/O quota, or ready-store capacity. Its slots count the
fixed service pool and legacy scheduler workers, not every process thread.
Later migration replaces this transitional ownership and ABI boundary
completely rather than retaining a compatibility wrapper.

## Relationship to ADR 0007

[ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.md)
preserves this decision's process-owned execution direction and ADR 0001
boundary while superseding the implicit details. It is authoritative for Run
identity and leases, monotonic terminal state, completion routing, target
`GraphRuntime` non-ownership, ledger token authority, commit races, graph/process
shutdown scope, and the issue #66–#76 dependency contract.
