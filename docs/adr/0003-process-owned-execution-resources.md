# ADR 0003: Execution Resources Are Process-Owned

## Status

Accepted as a target architecture. The current per-graph scheduler ownership
remains current behavior until the migration is implemented. The current
process-wide scheduler-worker admission ledger is a containment step, not an
implementation of this decision. ADR 0007 supersedes this ADR only as the
detailed target ownership and lifecycle contract; the high-level process
ownership decision and its historical context remain in force.

## Context

Each current `GraphRuntime` owns HP and RT scheduler instances, and scheduler
interfaces combine policy, worker lifecycle, queues, batches, device routing,
completion, and exceptions. Multiple graphs therefore multiply physical
threads and device contexts. Current software now limits that multiplication
with resolved per-instance worker grants and one conservative 32-slot process
ledger shared across embedded Hosts. The scheduler still cannot express
cross-Run fairness, process memory limits, cancellation, shared execution, or
resource admission beyond those scheduler-owned worker slots.

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

ADR 0001 remains fully in force. Once implemented, this decision supersedes the
physical resource ownership described by the current per-graph scheduler
sections of `docs/kernel-architecture/Scheduler-Architecture.md`; it does not
supersede the ready-task-only scheduler boundary.

The current containment contract accepts worker requests from zero through
eight, resolves zero to a nonzero grant capped at eight, and reserves at most
32 conservative scheduler-worker slots across all embedded Hosts. Graph load
reserves HP+RT together, replacement requires transient candidate headroom,
and move-only RAII owners release slots only after concrete scheduler
destruction. This prevents unbounded per-graph multiplication but leaves the
workers, queues, epochs, and policies inside each `IScheduler`.

`SchedulerWorkerBudget` is therefore not the target `ExecutionService` or
`ResourceLedger`: it owns no executor, Run identity, cancellation, fairness,
memory/device/I/O quota, or ready-store capacity, and its slots do not count all
process threads. The future migration replaces this transitional ownership and
ABI boundary completely rather than retaining a compatibility wrapper.

## Relationship to ADR 0007

[ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.md)
preserves this decision's process-owned execution direction and ADR 0001
boundary while superseding the implicit details. It is authoritative for Run
identity and leases, monotonic terminal state, completion routing, target
`GraphRuntime` non-ownership, ledger token authority, commit races, graph/process
shutdown scope, and the issue #66–#76 dependency contract.
