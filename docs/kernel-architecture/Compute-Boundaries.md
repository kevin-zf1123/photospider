# Compute Boundaries

## 1. Scope and ownership

| Owner | Owns | Boundary |
| --- | --- | --- |
| `GraphContext` | Copied workflow source, revision, and snapshot currentness | Does not own compiler output or execution workers |
| `Compiler` | Validation, immutable IR stages, plans, and stage identities | Does not own runtime Values, callbacks, or daemon state |
| `ExecutionRun` | One execution's ready steps, intermediates, cancellation state, and diagnostics | Does not own shared pools or durable results |
| `ExecutionContext` | CPU worker pool, optional native lane, frozen operation registry, caches, and resource ledger | Does not own graph mutation or daemon Jobs |
| Operation definition | Frozen traits and callback contract | Does not own execution capacity or publication authority |

## 2. Data and memory boundaries

`ExecutionPlan` contains copied operation metadata and demand geometry. It does not retain caller input addresses. `ExecutionBindings` supplies Values, regional sources, snapshots, or structural planar images for one Run. The Run retains those owners while callbacks may use them; each returned `Value` or `PlanarImage` retains its own backing and resource lease after the Run ends.

Estimated bytes guide planning and reservation. The resource ledger accounts admitted or allocated capacity according to the resource class; an estimate by itself is not an allocation lease. A `Region` describes logical coordinates, while `StridedLayout` maps those coordinates to byte addresses.

## 3. Execution boundary

Each `ExecutionRun` schedules its ready steps within `ExecutionOptions::maximum_parallelism`. CPU callbacks compete for the context's fixed CPU workers. Optional native callbacks use the configured backend lane. The context-wide waiting admission bounds callbacks that have not started; ordinary running callbacks release that waiting slot. A staged CPU job holds its slot until its tile callbacks retire and the job leaves the queue. See [parallel execution](Parallel-Execution-Model.md) for the lane and staged-work contract.

The public API exposes plans, bindings, results, statuses, and diagnostics. Queue entries, byte-ledger leases, native handles, and callback owners remain private implementation state. The daemon consumes the public package and owns IPC and Job lifecycle.

## 4. Limitations and non-goals

- A graph revision is compiler currentness state, not a daemon session identifier.
- A Run is a synchronous kernel call, not a public scheduling object.
- A backend label records placement intent or outcome, not a device handle.
- Result publication succeeds only after cancellation and graph-currentness checks.
- The kernel owns neither document persistence nor result recovery.
