# Scheduler Architecture

The kernel has a formal scheduler target interface and legacy runtime queues.
This document defines how to read the current implementation.

## Formal Target: IScheduler

`IScheduler` is the formal scheduler interface. A scheduler attaches to a
`GraphRuntime`, starts worker resources, schedules compute, and shuts down
cleanly.

Core lifecycle:

```text
create -> attach(runtime) -> start -> schedule(...) -> shutdown -> detach
```

Compute routes by `ComputeIntent`:

| Intent | Expected scheduler role |
| --- | --- |
| `GlobalHighPrecision` | Throughput-oriented HP compute. |
| `RealTimeUpdate` | Low-latency interactive update. |

`GraphRuntime` stores a scheduler map keyed by `ComputeIntent`.

In the current parallel runtime path, a `RealTimeUpdate` request can deliberately
submit both RT preview work and HP update work for the same dirty region. The RT
work preserves interactive feedback, while the HP work keeps the mission pool
and formal HP cache synchronized with the graph state. If this dual-submit path
creates blocking, starvation, or worker re-entrancy concerns, those concerns
belong in scheduler design: reserve or steal workers appropriately, use epochs
and cancellation for stale RT work, and avoid waiting policies that can deadlock
worker-owned execution. Do not solve this by making RT output a formal HP cache
source.

## Current Migration State

The implementation still contains worker queues, epochs, and task submission
APIs directly in `GraphRuntime`. Some compute paths still call
`ComputeService::compute_parallel` and submit tasks to those queues directly.

Treat these runtime queues as migration support, not the permanent scheduler
API. New scheduler-facing design should target `IScheduler`.

## Built-in Schedulers

| Type | Meaning |
| --- | --- |
| `cpu_work_stealing` | Multi-threaded CPU scheduler. |
| `serial_debug` | Deterministic single-threaded debugging scheduler. |
| `gpu_pipeline` | Heterogeneous CPU/GPU scheduler. |
| `heterogeneous` | Alias for `gpu_pipeline`. |

## Node-Level Scheduling

`IScheduler` includes node-level scheduling hooks such as `schedule_node`,
`schedule_nodes`, and task group aggregation helpers. These are intended to move
device selection and tile grouping decisions into the scheduler layer.

Default implementations may fall back to the legacy `schedule` path.

## Epoch and Cancellation

Runtime and scheduler queues use epochs to cancel stale queued work. Epoch `0`
is treated as non-cancelable compatibility work. New interactive scheduling
should assign real epochs so obsolete RT work can be dropped.

## Observability

`GraphRuntime::SchedulerEvent` records assignment, node execution, and tile
execution events. This is useful for validating scheduler behavior during the
migration.

## Development Direction

- Keep `IScheduler` as the formal public scheduler interface.
- Route more compute paths through scheduler instances over time.
- Avoid adding new permanent dependencies on `GraphRuntime` internal queues.
- Keep plugin scheduler lifecycle compatible with `Plugin-ABI.md`.
