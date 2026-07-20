# ADR 0003: Execution Resources Are Process-Owned

## Status

Accepted as a target architecture. Issue #68 implements the first current
process-execution slice: the embedded composition root explicitly creates and
injects one CPU `ExecutionService`, and built-in CPU non-realtime, non-dirty
full-HP Runs submit only ready, lease-backed work to its single-Run physical
worker domain. Transitional per-graph scheduler owners remain allocated for
all routes, and serial, GPU, plugin, dirty, and realtime execution still uses
them directly. The process-wide scheduler-worker admission ledger remains a
containment step rather than the target resource owner. Multi-Graph sharing,
general dirty/realtime lease coverage, resource accounting, policy, and
revision-safe commit remain target behavior. ADR 0007 supersedes this ADR only
as the detailed ownership and lifecycle contract; the high-level process
ownership decision and its historical context remain in force.

## Context

Each current `GraphRuntime` still owns HP and RT scheduler instances, and
scheduler interfaces combine policy, worker lifecycle, queues, batches, device
routing, completion, and exceptions. For the issue #68 built-in CPU full-HP
slice, private route metadata selects the injected `ExecutionService`, whose
concrete CPU runtime is reconfigured to the exact planned grant and executes
one complete Run at a time. The transitional Graph-owned CPU scheduler remains
alive and charged while that service route runs, so this slice intentionally
duplicates worker ownership until issue #69 removes per-Graph workers.

Current software limits the transitional multiplication with resolved
per-instance worker grants and one conservative 32-slot process ledger shared
across embedded Hosts. Neither that ledger nor the current single-Run service
expresses cross-Run fairness, process memory limits, cancellation, multi-Graph
shared execution, or resource admission beyond those scheduler-owned worker
slots.

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

ADR 0001 remains fully in force. Issue #68 partially supersedes the physical
ownership described by the per-graph scheduler sections of
`docs/kernel-architecture/Scheduler-Architecture.md`: only built-in CPU
non-realtime, non-dirty full-HP ready work executes on the injected service.
The remaining routes and transitional scheduler owners stay current, and the
ready-task-only boundary remains fully in force.

The current containment contract accepts worker requests from zero through
eight, resolves zero to a nonzero grant capped at eight, and reserves at most
32 conservative scheduler-worker slots across all embedded Hosts. Graph load
reserves HP+RT together, replacement requires transient candidate headroom,
and move-only RAII owners release slots only after concrete scheduler
destruction. This prevents unbounded per-graph multiplication but leaves the
workers, queues, epochs, and policies inside each `IScheduler`.

`SchedulerWorkerBudget` is therefore neither the current `ExecutionService` nor
the target `ResourceLedger`: it owns no executor, Run identity, cancellation,
fairness, memory/device/I/O quota, or ready-store capacity, and its slots do not
count the service pool or all process threads. Later migration replaces this
transitional ownership and ABI boundary completely rather than retaining a
compatibility wrapper.

## Relationship to ADR 0007

[ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.md)
preserves this decision's process-owned execution direction and ADR 0001
boundary while superseding the implicit details. It is authoritative for Run
identity and leases, monotonic terminal state, completion routing, target
`GraphRuntime` non-ownership, ledger token authority, commit races, graph/process
shutdown scope, and the issue #66–#76 dependency contract.
