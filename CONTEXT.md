# Photospider Kernel Context

Photospider is a graph-based image-processing runtime. The current active
bounded context covers graph state, compute planning, dirty-region propagation,
ready-task scheduling, cache, data ownership, and plugin contracts.

## Authoritative Language

Read `docs/kernel-architecture/Terminology.md` before naming kernel concepts.
That glossary defines the vocabulary of the current implementation, including:

- `ps::Host`, `Kernel`, `GraphRuntime`, and `GraphModel` ownership;
- graph-state operations and `GraphStateExecutor`;
- `ComputeIntent`, `ComputeRun`, `ComputePlan`, `DirtyRegionSnapshot`, and
  `ComputeTaskDispatcher`;
- `ReadyTaskSubmission`, `IScheduler`, cache, `ImageBuffer`, providers, and
  adapters.

Use the maintained domain documents indexed by
`docs/kernel-architecture/README.md` for observable behavior, implementation
boundaries, failure semantics, and source/test entry points. English documents
are authoritative; reader-oriented Chinese copies live under
`docs/kernel-architecture/zh/`.

## Current and Target Concepts

Issue #67 established one private, request-owned `ComputeRun` for each
non-realtime HP request. Issue #69 extended current behavior so realtime
requests create independent HP `Full` and RT `Interactive` child Runs without
creating a `RunGroup`; built-in CPU submissions from those Runs and from
multiple Graphs share one fixed Host-composed `ExecutionService`. Each Run
retains its descriptor, monotonic phase, exact terminal outcome, plan or dirty
staging, stable `ComputeRunLease` values, and
`(RunId, RunLocalTaskId)` completion identity.

Issue #70 makes the same service the current owner of one Host-authoritative
`ResourceLedger`, complete checked Run admission, and the entry/byte-bounded
ready store used by initial and dependency-released work. Issue #71 makes the
private Interactive/Throughput policy seam current: the store performs fixed
three-to-one class arbitration first, then applies dispatch-count aging,
deadline, and class-local Graph/Run fair-score ordering only within the selected
class. Its protected headroom account charges only active built-in Throughput
roots and follows exact ledger root lifetime; Interactive and transitional
legacy roots do not debit that class quota. Do not describe the remaining
accepted targets as current: issue #72 authoritative
`GraphRevision` commit validation, issue #73 cancellation, issue #74
supersession, issue #75 policy-only scheduler ABI generation, and issue #76
request-owned `RunGroup`, final `ExecutionService::RunLifecycleRegistry`,
graph-close/process-shutdown fence, and final lifecycle/telemetry invariants
remain future. The general `Value` model, heterogeneous executors, server
control plane, and isolated plugin workers also remain later target work.
The detailed Run/process-execution ownership decision is
`docs/adr/0007-compute-runs-and-process-execution-have-separate-owners.md`;
the combined accepted direction remains
`docs/roadmap/Kernel-Evolution.md`.

Long-lived decisions are recorded in `docs/adr/`. Task state, dependencies,
and acceptance evidence belong in GitHub Projects and Issues rather than this
context document.

## Non-Negotiable Distinctions

- A graph-state operation is not a scheduler task.
- A ready task is not a task graph.
- `ComputeIntent` is not resource policy or commit policy.
- `DirtyRegionSnapshot` is not `ComputeTaskGraph`.
- HP cache is not RT proxy state.
- `ImageBuffer` is not a general Tensor, Deep Image, or vector-scene model.
- A legacy worker-owning `IScheduler` is neither the current Host-composed CPU
  `ExecutionService` nor the target policy-only scheduler generation.
- A scheduler epoch is neither a `RunId` nor a completion identity. Current
  built-in CPU full, dirty, and preflight completion is scoped by one stable
  Run lease and `(RunId, RunLocalTaskId)`; only legacy dirty scheduler routes
  retain their synchronous borrowed-handle path.
- A target `RunGroup` coordinates results and lifecycle for independent HP/RT
  Runs; it is not a mixed-domain Run or a cross-domain task graph.
- A graph-lifetime lease protects a target Graph lifetime; it does not make
  `GraphRuntime` the owner of admitted Runs or their process registry.
