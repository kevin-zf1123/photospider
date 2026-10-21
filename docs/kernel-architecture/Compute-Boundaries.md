# Compute Boundaries

[Chinese reader version](zh/Compute-Boundaries.zh.md).

## 1. Scope and ownership

| Owner | Responsibility | Lifetime boundary |
| --- | --- | --- |
| `GraphContext` | Copied workflow source, revision and snapshot currentness | Source mutation invalidates source-bound versions; it does not mutate an active Run's bindings |
| `Compiler` | Static specialization, validated IR, optional optimization, physical lowering and validation | Produces immutable tables without executing sample payload |
| `ExecutionPlan` | CFG, Task/Tile/Call tables, layouts, placement, reads, selections and resource bounds | Shared immutable program, independent of runtime input addresses |
| `PreparedPlan` | Linked fixed kernels, concrete device compatibility, static resources and module owners | Shared across Runs in its originating context; topology stays fixed |
| Run and `RunHandle` | Immutable bindings, activation/task/call state, readiness, retirement, cancellation and diagnostics | Accepted work retains its owners through drain; join waits for task and delivery settlement |
| `ExecutionContext` | Bounded CPU workers, GPU credits, fair admission, frozen registry and resource root | Destruction stops admission and drains accepted Runs and callbacks |
| Operation definition | Static contracts, lowering, kernel preparation and numerical implementation | Declares all reads, writes, scratch and completion obligations before unchecked invocation |

## 2. Data and memory boundaries

`ExecutionBindings` supplies owning Result references. Submission captures their schema, coverage and storage authority for one Run. A later caller publication or binding replacement cannot widen that snapshot. Each dynamic produced value has a new identity; aliases of the same complete value retain its identity. Returned Results and owning read windows may outlive the Run and context by retaining their release contexts.

Physical tiles have unique declared allocations. A Region describes logical coordinates; an affine layout maps them to a fixed allocation view. Native preparation resolves actual capacity and alignment. Block admission reserves all output backing, per-lane scratch, completion and delivery capacity before its first task. A budget rejection returns `ResourceExhausted` without issuing that block.

Payload backing and managed metadata have separate observations. Context/PreparedPlan and existing input owners retain their original attribution; new Run metadata follows its originating resource root through cross-thread release. Reservation is not physical allocation, and neither counter describes RSS. See [managed resources](Managed-Resources.md).

## 3. Execution and completion

The planner expands all dependencies before execution. The coordinator issues any ready task that satisfies dependency, lane, context-credit, effect and failure-cut constraints. CPU workers execute fixed synchronous entries; accepted GPU submissions complete asynchronously. A successful task makes its outputs available to dependent tasks immediately. Calls retire in dynamic IR order, independently of task issue and completion order.

`CpuExact` requires CPU placement. `NativeGpu` is a static compilation choice: GPU-capable operations receive GPU lowering requests and CPU-only operations receive CPU requests. An operation may explicitly mix CPU/GPU tasks within one call when its traits permit them. COPY tasks implement declared transfers; Device endpoints require GPU COPY. Preparation rejects unavailable placements without rewriting tasks, and execution never retries on another backend.

Each block activation consumes the bounded `maximum_block_activations` and `maximum_work` budgets; even an empty loop consumes work. Cancellation and source currentness are checked at coordinator safe points. Task graphs, schemas and dependencies do not expand during execution. Numerical work obeys the operation's prepared contract rather than a continuation work-service protocol.

Rejection leaves invocation ownership with the coordinator and produces no completion. Acceptance retains input/output/scratch, parameters, kernel/module owners and the reserved completion record until exactly one terminal event establishes quiescence and visibility. Cancellation suppresses new work and drains accepted work. A timeout cannot authorize early buffer release.

Ordered call delivery runs outside the coordinator on a separate serial lane per Run. Observer failure aborts the Run and stops later delivery. Final RET publication waits for task quiescence, delivery settlement and the final cancellation/currentness check. Already delivered owning values remain valid. See [parallel execution](Parallel-Execution-Model.md) for failure priority, fairness, timing and native measurements.

## 4. Public boundary

The public API exposes immutable Plan/preparation values, bindings, `RunHandle`, cancellation, join, owning Results and independent observations. Queue entries, native device handles and mutable scheduler state remain internal. A handle is process-local; the daemon separately owns IPC and Job lifecycle.

The kernel provides no durable Run, document persistence, result recovery, runtime dependency discovery, payload cache hierarchy or implicit migration. Provider code is trusted and must end its accesses before reporting completion.
