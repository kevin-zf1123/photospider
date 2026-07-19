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

The current issue #67 slice gives each non-realtime HP request one private,
request-owned `ComputeRun`. It captures an opaque id, the session label, a
topology-only submission revision, target, HP intent, full quality, explicit
QoS, monotonic phase, and one terminal outcome. Its shared control state owns
the full scheduler submission plan and temporary results, or the standalone
dirty HP staging buffer. The scheduler-backed full HP path retains that state
through non-forgeable `ComputeRunLease` values, executes owned callbacks, and
routes task failure by `(RunId, RunLocalTaskId)`.

Do not describe the remaining accepted future objects as if they already
exist. Request-owned `RunGroup`,
`ExecutionService::RunLifecycleRegistry`, process-owned `ExecutionService`,
authoritative `GraphRevision` commit validation, `ResourceLedger`, the general
`Value` model, heterogeneous executors, server control plane, and isolated
plugin workers remain target-only until their implementation lands.
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
- The current worker-owning `IScheduler` is not the target process execution
  domain.
- A scheduler epoch is neither a `RunId` nor a completion identity. Current
  scheduler-backed full HP completion is scoped by one stable Run lease and
  `(RunId, RunLocalTaskId)`; request-local dirty executors still use their
  separate synchronous borrowed-handle path.
- A target `RunGroup` coordinates results and lifecycle for independent HP/RT
  Runs; it is not a mixed-domain Run or a cross-domain task graph.
- A graph-lifetime lease protects a target Graph lifetime; it does not make
  `GraphRuntime` the owner of admitted Runs or their process registry.
