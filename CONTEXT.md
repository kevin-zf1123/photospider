# Photospider Kernel

Photospider kernel language for graph state ownership, compute planning, and
scheduler dispatch boundaries.

## Language

**GraphRuntime**:
The per-graph runtime boundary that holds one graph's state container and
runtime resources. It is not a compute task queue owner.
_Avoid_: worker queue runtime, compute queue owner

**GraphModel**:
The in-memory graph state, including nodes, graph topology, cache metadata, and
per-graph runtime metadata.
_Avoid_: runtime, scheduler state

**Graph-State Operation**:
An operation that reads or mutates graph state without deriving compute tasks,
such as graph loading, cache commands, inspection, and ROI projection.
_Avoid_: compute task, scheduler job

**Graph-State Access Boundary**:
The boundary through which graph-state operations read or mutate one graph's
state without becoming scheduler-dispatched compute work.
_Avoid_: scheduler dispatch, compute task queue

**Kernel Interaction Boundary**:
The public `ps::Host` seam is the only supported boundary for kernel-external
information exchange. CLI, TUI, frontend, debug tooling, and external tests use
`ps::Host` APIs for inspection, trace/event draining, dirty snapshots,
scheduler status, and command results. The embedded Host adapter delegates
those requests to the internal `InteractionService`/`Kernel` boundary; callers
outside the backend do not reach `GraphRuntime` or internal services directly.
_Avoid_: direct `InteractionService` access from frontends, direct runtime
inspection, internal service bypass, test-only runtime access

**Per-Graph Exclusive Access**:
The default graph access semantics where compute and graph-state operations do
not concurrently read or mutate the same graph state.
_Avoid_: mixed graph mutation, opportunistic shared graph access

**Compute Commit Policy**:
The policy that decides whether compute writes directly to visible graph state
or stages outputs before committing them.
_Avoid_: ComputeIntent, scheduler type

**DirectGraphCommit**:
The compute commit policy where compute updates visible graph state during the
request and graph-state operations wait for the compute request to finish.
_Avoid_: interruptible commit

**StagedInterruptibleCommit**:
A future compute commit policy where compute stages outputs outside the visible
graph state and graph-state operations may request cancellation before commit.
_Avoid_: realtime intent, scheduler cancellation

**ComputeTaskPlanner**:
The planning boundary that derives request-scoped `ComputePlan` and
`ComputeTaskGraph` semantics from a compute request and graph topology.
_Avoid_: scheduler, task executor, dirty queue owner

**ComputePlan**:
A request-scoped static compute analysis for one compute domain. It owns the
full planned compute-task shape for the request; dirty updates only select from
that shape.
_Avoid_: dirty snapshot, scheduler queue

**DirtyRegionSnapshot**:
Graph-scoped dirty-region state, alongside graph topology, that records
dirty source membership, dirty-node lifecycle state, and propagator-derived
affected regions, tiles, and mappings for a graph.
_Avoid_: scheduler graph, task graph, traversal adjacency

**DirtyRegionNode**:
A graph node that emits dirty-region lifecycle and ROI state. Frontend input or
compute results may cause the node to emit dirty state, but dirty regions are
always node-originated.
_Avoid_: interaction source, scheduler trigger

**DirtyControlLane**:
The serialized graph-scoped control path that accepts dirty-region lifecycle
updates from dirty nodes and refreshes dirty source state.
_Avoid_: scheduler task queue, node-owned compute queue

**ReadyTaskSubmission**:
A scheduler-visible submission of concrete ready task callbacks. The executor
controls submission order and may attach scheduler-supported metadata such as
epoch or optional priority hints, but scheduler correctness does not depend on
a global priority contract or on receiving a task graph.
_Avoid_: task graph submission queue, dirty-source queue, scheduler-owned
planner

**ComputeTaskGraph**:
The request-scoped compute-task dependency graph derived from graph topology
for one compute domain. It enumerates the real node and tile tasks available to
the request.
_Avoid_: dirty-region state, scheduler queue, lazy tile template

**DirtyUpdateWorkSet**:
The per-update subset of compute tasks selected from a `ComputeTaskGraph` by
the current `DirtyRegionSnapshot`, dirty ROI, and dirty-node lifecycle state.
_Avoid_: static compute plan, graph topology, scheduler policy, task expansion

**DirtySourceTaskCollector**:
The executor-side collector that turns dirty source lifecycle/source ROI state
into source ready tasks. These tasks can compute source output and write cache,
but they stay outside downstream `ComputeTaskGraph` dependency lifecycles.
_Avoid_: dirty scheduler queue, node-owned compute queue

**TaskGraphReadyChecker**:
The executor-side dependency checker that owns `ComputeTaskGraph` dependency
counters, dependent maps, and ready release for graph tasks.
_Avoid_: scheduler graph executor, dirty region propagator

**DirtyWorkPruner**:
The executor-side selector that clips an already-planned `ComputeTaskGraph`
into a downstream dirty work set using propagator-derived actual dirty regions.
It selects existing tasks; it does not create new node or tile task shapes.
_Avoid_: tile expander, compute task planner, dirty source collector

**ComputeTaskDispatcher**:
The renamed execution orchestrator that replaces the current
plan-execution class after the task-collection split. It runs a `ComputePlan`
by coordinating task collectors, ready checking, dirty pruning, scheduler
ready-task dispatch, and result commit. It owns dispatcher runtime state; it
does not transfer task graph ownership to the scheduler.
_Avoid_: old executor naming after split, parallel-only executor, scheduler task
graph owner, priority authority

**SchedulerTaskRuntime**:
The scheduler-owned dispatch boundary for already-planned compute tasks.
_Avoid_: graph runtime queue, graph-state command executor
