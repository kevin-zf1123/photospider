# Graph state access is not scheduler dispatch

Graph-state operations such as YAML loading, cache commands, inspection, and
ROI projection should execute through an explicit graph-state access boundary,
not through `SchedulerTaskRuntime`. `SchedulerTaskRuntime` remains the
scheduler-owned dispatch boundary for already-ready compute task callbacks. It
does not receive task graphs, maintain graph dependency counters, derive dirty
work sets, or mutate dirty-region snapshots. Dependency accounting,
source-first dirty task release, task pruning, and ready-task submission stay in
the compute-service dispatcher boundary, while graph-state access is protected
separately by `GraphStateExecutor` so scheduler implementations do not become
responsible for non-compute commands.

The current default is per-graph exclusive access: compute requests and
graph-state operations do not concurrently read or mutate the same visible
`GraphModel`. `Kernel` submits synchronous, asynchronous, image-returning, and
scheduler-backed parallel compute requests through `GraphStateExecutor`.
Parallel compute may still use `SchedulerTaskRuntime` for ready task callbacks,
but the outer request keeps the graph-state access boundary until the compute
service commits coherent visible graph state.

A later `StagedInterruptibleCommit` policy may allow long-running compute to
stage outputs outside visible graph state and let graph-state operations request
cancellation before commit. That policy is intentionally separate from
`ComputeIntent`, which remains HP/RT semantic intent rather than a concurrency
or commit-mode selector.
