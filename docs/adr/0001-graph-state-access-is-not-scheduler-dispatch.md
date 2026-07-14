# ADR 0001: Graph-State Access Is Not Scheduler Dispatch

## Status

Accepted.

## Context

Graph document loading, cache commands, inspection, ROI projection, and graph
mutation read or modify `GraphModel`, but they are not compute callbacks.
Treating them as scheduler work would make scheduler implementations depend on
graph ownership, persistence, dirty propagation, and mutation semantics.

Compute execution has a different responsibility split. The compute subsystem
derives a task graph, owns dependency counters and dirty work selection, and
releases concrete work only after its dependencies are satisfied.

## Decision

Graph-state operations execute through an explicit per-graph graph-state access
boundary, currently `GraphStateExecutor`. They do not enter
`SchedulerTaskRuntime`.

`SchedulerTaskRuntime` receives only concrete ready compute callbacks. It does
not receive task graphs, maintain graph dependency counters, derive dirty work
sets, mutate dirty-region snapshots, or commit graph state.

Dependency accounting, source-first dirty task release, task pruning, and
ready-task submission remain in the compute dispatcher. The current default is
per-graph exclusive access: graph-state operations and compute requests do not
concurrently read or mutate the same visible `GraphModel`. A parallel request
may dispatch ready callbacks to scheduler workers while its outer request keeps
the graph-state boundary until coherent commit.

Compute commit policy remains separate from `ComputeIntent`. A staged,
interruptible policy may change how visible commit and cancellation interact
without turning graph-state operations into scheduler work.

## Consequences

- Scheduler implementations remain testable without Graph, persistence, ROI,
  or cache ownership.
- Graph mutation ordering has one explicit owner.
- Parallel callback execution does not imply concurrent mutation of visible
  graph state.
- A future process-wide execution domain must preserve the same ready-task-only
  input boundary.
- More concurrent graph editing requires an explicit revision and staged-commit
  model rather than routing mutations through the compute scheduler.

## Relationship to Other Decisions

ADR 0003 changes physical execution-resource ownership but preserves this
decision. No decision supersedes ADR 0001.
