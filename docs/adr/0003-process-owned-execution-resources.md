# ADR 0003: Execution Resources Are Process-Owned

## Status

Accepted as a target architecture. The current per-graph scheduler ownership
remains current behavior until the migration is implemented.

## Context

Each current `GraphRuntime` owns HP and RT scheduler instances, and scheduler
interfaces combine policy, worker lifecycle, queues, batches, device routing,
completion, and exceptions. Multiple graphs therefore multiply physical
threads and device contexts, while the scheduler still cannot express
cross-Run fairness, process memory limits, cancellation, or global admission.

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
