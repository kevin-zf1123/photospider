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
requests create independent HP `Full` and RT `Interactive` child Runs; issue
#74 now composes those children in one private request-owned `RunGroup` without
creating a mixed-domain Run. Built-in CPU submissions from those Runs and from
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
legacy roots do not debit that class quota. Issue #72 makes strong non-reused
Graph instance identity, checked nonzero authoritative `GraphRevision`,
request-owned product snapshots, exact-revision staged publication, and the
separate compute-request lane current. Issue #73 adds private request
cancellation, exact Run settlement, and the one-shot commit contender. Issue
#74 adds per-live-Graph latest-wins supersession: missing intent canonicalizes
to HP, every candidate receives a graph-wide checked generation, same-key
pending work coalesces behind one reserved ticket, and exact generation joins
instance/revision validation before visible commit. RT publishes before opening
its `RunGroup` sibling gate, and later stale HP or failed newer work cannot roll
back an already valid RT proxy. Do not describe the remaining accepted targets
as current: issue #75 policy-only scheduler ABI generation and issue #76 final
`ExecutionService::RunLifecycleRegistry`, graph-close/process-shutdown fence,
and lifecycle/telemetry invariants remain future. The general `Value` model,
heterogeneous executors, server control plane, and isolated plugin workers also
remain later target work.
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
- A request-owned `RunGroup` coordinates results and cancellation for
  independent HP/RT Runs; it is not a mixed-domain Run or a cross-domain task
  graph.
- A graph-lifetime lease protects a target Graph lifetime; it does not make
  `GraphRuntime` the owner of admitted Runs or their process registry.
