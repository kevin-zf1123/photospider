# Compute Boundaries

## 1. Scope and ownership

| Owner | Owns | Boundary |
| --- | --- | --- |
| `GraphContext` | Copied workflow source, revision, and snapshot currentness | Does not own compiler output or execution workers |
| `Compiler` | Validation, immutable IR stages, static mapper calls, tile expansion into plan instructions, plan validation, and stage identities | Does not own runtime Tensor backing, run-time callbacks, or daemon state |
| `ExecutionRun` | One execution's program counter, block values and parameters, fixed-grant bindings, dependency evidence, cancellation state, work budget, and diagnostics | Does not own shared pools, durable results, dependency mapping, or instruction order |
| `ExecutionContext` | CPU worker pool, optional native lane, frozen operation registry, caches, and resource ledger | Does not own graph mutation or daemon Jobs |
| Operation definition | Frozen traits, static dependency mapper, and callback contract | Does not own execution capacity, read grants beyond its compiled mapping, or publication authority |

## 2. Data and memory boundaries

`ExecutionPlan` retains static metadata, member tile geometry and the complete tile instruction program, not caller addresses. `ExecutionBindings` supplies owning Result references for one Run. Each Result consists of static Tensor members; returned Results and read windows retain backing and leases after Run completion.

Estimated bytes guide planning and reservation. The resource ledger accounts admitted or allocated capacity according to the resource class; an estimate by itself is not an allocation lease. A `Region` describes logical coordinates, while `StridedLayout` maps those coordinates to byte addresses.

## 3. Execution boundary

The planner performs all dependency expansion before execution. Each `ExecutionRun` executes the plan's instructions one at a time in stored order and only invokes the fixed services each instruction needs: grant binding, optional cache lookup, operation start and poll, publication validation, and block transitions. It does not call mappers, create producer tasks, derive tile mappings or reorder instructions. Every block entry, instruction, grant assertion and evidence merge consumes `ExecutionOptions::maximum_dependency_work`, and operation work charges consume the Root work budget; both paths check cancellation and currentness, so empty loops and long instruction streams stay bounded. Exhaustion returns `ResourceExhausted`. CPU callbacks compete for the context's fixed CPU workers. Optional native callbacks use the configured backend lane. The context-wide waiting admission bounds callbacks that have not started; ordinary running callbacks release that waiting slot. A staged CPU job holds its slot until its tile callbacks retire and the job leaves the queue. See [parallel execution](Parallel-Execution-Model.md) for the lane and staged-work contract.

The public API exposes plans, bindings, results, statuses, and diagnostics. Queue entries, byte-ledger leases, native handles, and callback owners remain private implementation state. The daemon consumes the public package and owns IPC and Job lifecycle.

## 4. Limitations and non-goals

- A graph revision is compiler currentness state, not a daemon session identifier.
- A Run is a synchronous kernel call, not a public scheduling object.
- A backend label records placement intent or outcome, not a device handle.
- Result publication succeeds only after cancellation and graph-currentness checks.
- The kernel owns neither document persistence nor result recovery.
