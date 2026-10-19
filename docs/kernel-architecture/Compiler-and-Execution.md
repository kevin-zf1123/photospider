# Compiler, Plan and Executor

Status: Proposed breaking design. This page and [Parallel execution](Parallel-Execution-Model.md), [Tile storage](Cache-Model.md) and [Dependencies](Dependency-Data.md) replace the execution design. Interfaces below are specification sketches, not installed APIs. Production C++ still implements the sequential continuation executor. The [reference model](prototypes/ooo_executor.py) exercises the new scheduling semantics independently. [Chinese reader version](zh/Compiler-and-Execution.zh.md).

## Scope and ownership

The compiler produces a complete physical program. The executor issues ready tasks to CPU workers and GPU queues, consumes completion events, and retires calls in IR order. Operators have an unchecked execution contract: callers supply valid numerical inputs; implementations honor declared accesses, numerical semantics and completion obligations.

| Owner | Responsibility |
| --- | --- |
| Frontend and IR validator | Source semantics, SSA, dominance, fixed schemas, control flow and call retirement order |
| Planner and Plan validator | Task expansion, fixed reads, unique tile producers, placement, transfers, resource bounds, dependencies and call membership |
| Loader and prepare | Bounded decoding, contract/ABI linking, concrete device compatibility and immutable kernel bindings |
| Run coordinator | Readiness, admission, completion, retirement, cancellation and ownership |
| CPU/GPU implementation | Numerical behavior, declared reads/writes, bounded scratch and actual completion with visibility |
| Host | Immutable per-Run bindings, result consumption and external orchestration |

The kernel retains the component boundary in [ADR 0015](../adr/0015-breaking-product-boundary-scope-reset.md). Daemon IPC and ephemeral jobs remain external. Structural validation occurs before issue. Execution checks operational failures and its own state; it does not scan samples, invoke numerical validators, certify output content or rediscover dependencies through `Need`.

## Program stages and semantic order

```text
WorkflowDocument / IR builder
              |
         ValidatedIR -- optional named optimizers --> ValidatedIR
              |                                           |
              +---------------- named planner -------------+
                                    |
                               ExecutionPlan <--- checked .psxp import
                                    |
                              prepare(device)
                                    |
                               PreparedPlan
                                    |
                         Run + immutable bindings
                            /               \
                       CPU pool          GPU queues
                            \               /
                             completion events
                                    |
                            ordered retirement
```

`FrontendRegistry`, `OptimizerRegistry` and `PlannerRegistry` retain named registration and frozen shared ownership. Planning remains independent of optional optimization. A planner builds task/tile/call/block tables through a private builder; the compiler validates the whole result before publishing it. This replaces the instruction-vector-only builder. Unknown names fail `NotFound`; invalid generated programs fail `Internal`; malformed imported programs fail `InvalidArgument`; incompatible contracts fail `TypeMismatch`.

`IRProgram::calls` is a declaration table. Each ControlFlow block's call list defines retirement ordinals. DependencyPaths deduplication gives repeated requests for one Call ID one call site; different Call IDs remain different invocations. Consecutive parameterless path blocks with unconditional single-predecessor/single-successor edges are coalesced into a scheduling block, preserving first-request order. Artificial DAG path boundaries must not serialize independent work. Branches, loop headers and merges retain explicit boundaries.

Within a block, data uses refer to earlier calls, incoming values or an acyclic graph of tasks within the same call. A task never depends on a later call's output. Physical task table order is independent of call order. Pure tasks may issue and complete out of order. Retirement is a contiguous prefix of the dynamic call sequence. Successful tile completion immediately enables internal consumers, even before its call retires.

Each retained IR call has a `CallDesc`, including pure calls with zero selected tasks. Zero-task calls retire as `Elided`, with no computed values. Output selection therefore preserves call-record order without claiming omitted work ran. Effects and required control dependencies remain mandatory.

## Physical plan

These C++17 declarations describe logical records, not a native-memory wire format. IDs index immutable Plan tables; `Slice` indexes a specified auxiliary array. Zero is a valid index; `none` is reserved.

```cpp
#include <cstdint>
#include <limits>
namespace ps_plan_design {
using Id = std::uint32_t;
constexpr Id none = std::numeric_limits<Id>::max();
struct Slice { Id offset, count; };
enum class TaskKind : std::uint8_t { Kernel, Copy, Effect };
enum class EndKind : std::uint8_t { Jump, Branch, Return };
struct TileDesc {
  Id schema, member, layout, domain;
  std::uint64_t tile_index, bytes, alignment;
  Id producer;                    // none for an incoming immutable value
};
struct ReadDesc { Id port, tile, view; };
struct TaskDesc {
  TaskKind kind;
  Id call, kernel, placement;
  Slice reads, writes, schema_reads, parameters;
  Slice predecessors, successors;
  Id scratch;                     // per-domain resource vector
};
struct CallDesc {
  std::uint64_t source_call;
  Id ordinal;
  Slice tasks, returns;            // task IDs and selected result descriptors
  Id effect_task;                 // none or one terminal effect task
};
struct BlockDesc {
  Slice tasks, calls, arguments, live_ins;
  EndKind end;
  Id condition;                   // host-readable UInt8 scalar, or none
  Slice edges, return_values;
  Id reservation;
};
struct EdgeDesc {
  Id target_block;
  Slice arguments, live_values;   // value IDs captured simultaneously
};
}  // namespace ps_plan_design
```

Auxiliary tables contain schemas, member grids, layouts, parameter bytes, immutable resource identities, kernel symbols, placement requirements, resource vectors and ID lists. A `ValueDesc` associates an IR Result with its schema and ordered `(member, tile index, TileId)` entries. Edge arguments and returns refer to those values. A read view records the checked logical extent, byte extent and strides. Schema-only reads reference static metadata and create no payload edge.

A task can produce several independent tiles, including several call outputs. All become available at that task's successful completion. Static shared stages become ordinary tasks with private SSA tile outputs and explicit consumers, instantiated once per call activation. Persistent temporary data is an SSA tile; task-local temporary data is scratch. Hidden allocations are outside the bounded operation contract.

Each materialized output tile has one producer and a distinct allocation. Fan-out shares that tile's immutable handle. `Copy` produces a new tile in the specified domain. A block argument renames an existing immutable value; it does not materialize another tile. A multi-tile Result is a collection of owning handles, without automatic dense assembly.

`prepare_kernel(symbol, static_metadata, parameters, resources, device)` produces an immutable `PreparedKernel` with direct entry points, decoded layouts, resource owners and pipelines. PreparedPlan retains these contexts and their modules; each invocation borrows its context through completion. Context destruction occurs after all referencing Runs drain.

`Placement` fixes backend class, numerical profile, memory domain and queue capability. `prepare` binds these to concrete device objects and kernels. It can reject an unavailable backend but cannot change task topology, implementation profile or COPY edges. CPU/GPU alternatives require separately compiled Plans. No implicit retry or placement fallback occurs after submission.

## A concrete scheduling block

This assembly notation is explanatory; the semantic prototype constructs the equivalent Task records directly and is not a `.psxp` parser. Durations are model inputs, not Plan scheduling guarantees.

```text
block entry:
  retire [call A, call C, call B, call D, call E]
  tile a, b, c, d, e: u8[4], host, separate-allocation
  task t0: call A, kernel @A, cpu, reads [],     writes [a], deps []
  task t1: call C, kernel @C, cpu, reads [a],    writes [c], deps [t0]
  task t2: call B, kernel @B, cpu, reads [],     writes [b], deps []
  task t3: call D, kernel @D, cpu, reads [b],    writes [d], deps [t2]
  task t4: call E, kernel @E, cpu, reads [c,d],  writes [e], deps [t1,t3]
  ret result = [e]
```

The compiler emits successors `t0:[t1], t1:[t4], t2:[t3], t3:[t4], t4:[]` and call membership counts `[1,1,1,1,1]`. With two CPU lanes, t0 and t2 issue together. No dependency is added from t1 to t2 merely to preserve the retirement list. A GPU variant inserts distinct host-to-device and device-to-host COPY tiles/tasks and changes placement before prepare; execution does not infer these copies.

Member tile geometry retains one positive power-of-two plan extent, default 128, and the spatial/batch/atomic-axis rules of ADR 0023. Grid geometry determines logical tiles; independent physical allocations determine storage. An RGBA Float32 tile with 128 by 128 pixels contains 262144 bytes, regardless of call/task numbering.

## Static construction and validation

The planner closes reads from selected returns, conditions, edge arguments, live-through values and mandatory effects. It invokes pure static mappers during compilation only. Data-dependent sampling declares a finite conservative superset. An arbitrary gather may require every source tile. The runtime never expands that dependency set.

Before publishing a Plan, the validator establishes:

1. Valid indices, slices, sizes, alignment and checked arithmetic; bounded parser/planner work; compatible schemas; SSA dominance and valid CFG.
2. One producer per materialized output tile, compatible layouts/domains, and valid read views. Each input has a local producer or declared incoming value.
3. An acyclic block task graph. Predecessors are deduplicated producer IDs; successors are their exact transpose; initial pending count equals predecessor count.
4. Tasks partition into exactly one call and block each. Call ordinals are contiguous. A producer's call ordinal is no greater than its consumer's. Task array position imposes no execution order.
5. Each effectful call has one terminal effect task after all its pure preparation tasks. The effect has no outgoing edge to preparation in that call and is eligible only at retirement head.
6. Required conditions, returns, edge arguments and dominating live-through values exist on the executing path. Only a chosen successor receives a dynamic activation.
7. Resource vectors cover outputs, persistent state, descriptors, command metadata, queue/completion records, delivery records and scratch. The backend supports the declared synchronization.

These checks validate declarations, not arbitrary native implementation code. Writing every declared output byte before success and obeying numerical semantics are operator obligations. Undeclared accesses or invalid numerical preconditions violate the unchecked contract; diagnostics are not guaranteed.

`select_outputs` closes a subset over existing tables, preserving control/effects and call order. It rebuilds task membership, adjacency, return holds and resource bounds. It neither calls a mapper nor adds missing tiles. A request outside compiled coverage requires compilation. An empty selection still executes control and effects.

## Run, control flow and return interface

`ExecutionPlan` owns serializable immutable program data. `PreparedPlan` owns linked kernels, device objects and module/resource leases. `RunHandle` owns mutable state and immutable binding leases, and retains its prepared program through drain. Concurrent Runs share prepared code and context pools with distinct output allocations.

The logical API is `prepare(plan, device_context)`, `submit(prepared, bindings, options)`, `request_cancel(run)` and `join(run)`. Each fallible operation returns typed status. `join` waits for all accepted submissions to quiesce, then returns the complete named Result set or one terminal failure. Result owners can outlive the Run and context by retaining their allocation release context.

An optional `on_call_retired(CallRecord)` receives sequence number, activation, source call, status and selected exported values. A healthy observer receives one record per retired call, including elided calls. Intermediate values omitted from export have no delivery hold. Records are stored in a bounded delivery queue reserved at block admission; user callbacks run outside the coordinator, independently of completion ingestion.

Each Run has one non-reentrant delivery lane. Callback n+1 starts only after callback n returns normally, so both invocation and normal-return order follow retirement order. Different Runs may deliver concurrently. `join` waits for task drain and delivery settlement; a callback must not synchronously join its own Run. A slow callback can delay join or consume capacity needed by the next block, but cannot stop CPU/GPU completion ingestion.

A callback exception closes that Run's delivery lane immediately: no later callback starts, queued records are discarded with their delivery holds released, and independently retained user values remain valid. Lane closure and enqueue are serialized. Enqueue to a closed lane rejects the record without taking its owners; the coordinator releases those delivery holds and marks the record settled, including retirement racing the failure notification. The delivery lane publishes one reserved settlement event that atomically carries `OperationFailed`, observer origin and the offending CallSeq. The failing callback record remains unsettled until the coordinator consumes that event; the coordinator records the abort before marking that record settled, in the same state transition. A success acknowledgment followed by a separate error event is forbidden. The coordinator stops further admission/retirement and drains accepted work. Retirement may already have enabled later effects before the callback failed; those effects cannot be retracted.

Final RET data remains internally held until all accepted work has quiesced and the coordinator has acknowledged settlement of every retired record, including the last callback. With no observer, record settlement is immediate. The coordinator consumes pending delivery/stop events and latches the terminal outcome at one serialized finalization point; only success transfers the final RET owners to the caller. An exception from the last callback therefore prevents successful RET even if all kernels have completed. Cancellation observed before that point retains its documented precedence; requests after it cannot alter the outcome. The prototype models owning retirement records directly and does not yet implement this callback protocol.

Block entry creates all task/call state for a fresh monotonic activation. A successful terminator runs after all block calls retire and all tasks complete. The coordinator reads the host-visible condition, captures every chosen edge argument and live-through value simultaneously, obtains next-frame holds, then releases the previous frame. Compiler liveness includes dominating values used beyond the immediate successor, even without explicit block arguments. Unchosen successors are not instantiated.

Loops create new materialized tile identities per activation. Incoming values retain their old allocation through their final read. Every block entry consumes bounded work, including empty loops, and rejects activation/sequence overflow. A device-resident branch scalar requires an explicit host COPY. Cancellation and currentness are checked at coordinator safe points.

Named RET values publish only after successful execution, delivery settlement and the final stop/currentness check. Already retired call results remain valid if a later call fails. Source-bound Plans retain graph-currentness semantics; standalone imported Plans are source independent. A host/Demand facade owns binding replacement: it invalidates an obsolete Run's generation and requests abort, then starts a new Run. Active tasks keep their immutable binding snapshot and owners through drain.

## Compiler artifacts

The proposed headers are `psir 5` and `psxp 5`. IR changes because call order means retirement and effects are explicit. Plan stores all physical tables, unchecked semantic mode, source identity, kernel contract versions, output selections and resource bounds. Canonical field encoding is independent of C++ padding. It contains no process pointers, native handles, Run state or cache contents.

Import performs bounded parsing, structural validation, symbol/resource linking and immutable publication. It runs no frontend, optimizer, mapper or planner. Import restores only static Plan data and resolves static contract/resource identities. Device object construction occurs exclusively in the subsequent explicit prepare stage. Digests identify content, not trust in native code. New IR, Plan and operation-manifest identity domains are required. v4 artifacts are rejected with a recompile diagnostic; there is no compatibility translation.

| Boundary | Replacement | Required implementation change |
| --- | --- | --- |
| IR order | Ordered retirement and dependency-driven issue | IR semantics, optimizer legality and identity |
| PlanStep/PlanTileInstruction | Task/Tile/Call/Block tables | Builder, validation, selection and codecs |
| Continuation ABI 7 | Fixed invocation and asynchronous backend completion | Proposed ABI 8 and traits 30; rebuild plugins, no continuation shim |
| Need/Publication/certification | Static reads and preallocated writes | Remove polling, grant assertions and automatic numerical certification |
| Shared stages/checkpoints/blocks | SSA intermediate tasks and scratch | Remove executor checkpoint/block/result reuse |
| Residency/fallback | One domain per tile, explicit COPY | Remove implicit mirror caches and automatic retry |
| Prefix observer | Ordered CallRecord and final RET | Replace per-tile prefix notification |
| Package/data schema | Independent version axes | Assign package/SOVERSION at implementation; revise schema encoding if publication-scope fields are removed |

The execution-specific portions of ADR 0016 (continuations/prefix notifications), ADR 0018 (runtime caches), ADR 0019 (implicit residency), ADR 0021 (storage reuse exceptions), ADR 0022 (staged ABI) and ADR 0023 (Need/certification/evidence reuse) are replaced in this proposed design. Compatible input ownership, fixed-grid geometry and product boundaries remain. Existing ADR acceptance does not imply acceptance or implementation of this proposal. Other architecture pages describe the current C++ implementation until migrated.

## Prototype and implementation acceptance

Run `python3 docs/kernel-architecture/prototypes/ooo_executor.py` for the semantic model and add `--test` for its behavioral checks. Logical event durations and distinct byte buffers exercise explicit/read-derived dependencies, failure cuts and task-error attribution, separate retirement/RET holds and retirement. It models one block, with a separate simultaneous-edge-transfer helper. It does not execute a complete CFG, implement delivery callbacks, model resource vectors/metadata capacity, or provide native backends, a public artifact parser or performance measurements.

Production acceptance requires compiler-to-Plan-to-CPU execution with real overlap, malformed artifact rejection, selection/loop coverage, asynchronous native GPU completion, explicit transfers, cancellation/failure owner drain, ordered observer delivery and resource accounting after export release. Physical GPU execution is required for native claims. Installed-consumer testing remains suspended during kernel development.

## Limitations

Shapes and task graphs per block are fixed. Branch prediction, cross-iteration speculation, runtime dependency discovery, fixed-size task ROBs, payload cache hierarchies, automatic recomputation, live tile aliasing and implicit migration are excluded. A callback that never terminates or a device that cannot establish quiescence can delay drain indefinitely; timeout never authorizes early buffer release.
