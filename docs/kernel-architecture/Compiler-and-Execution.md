# Compiler, Plan and Executor

Status: Proposed breaking design. This page and [Parallel execution](Parallel-Execution-Model.md), [Tile storage](Cache-Model.md) and [Dependencies](Dependency-Data.md) replace the execution design. The tables below describe the target contract. The C++ public API executes physical CPU programs and Host-domain asynchronous GPU entry points through `prepare`, `submit` and `join`. Native Shared storage supports mapped CPU access and explicit Host/Shared COPY tasks. Metal MSL and Vulkan SPIR-V kernels are prepared once and complete asynchronously on the native device. Device-private allocations support native kernels, ordered Result publication and explicit Device input contracts. Explicit GPU COPY tasks connect Host, Shared and Device storage with byte-exact affine reads. The [reference model](prototypes/ooo_executor.py) exercises the new scheduling semantics independently. [Chinese reader version](zh/Compiler-and-Execution.zh.md).

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

`FrontendRegistry`, `OptimizerRegistry` and `PlannerRegistry` retain named registration and frozen shared ownership. Planning remains independent of optional optimization. A planner builds task/tile/call/block tables through a private builder; the compiler validates the whole result before publishing it. This replaces the instruction-vector-only builder. Unknown names fail `NotFound`; invalid generated programs fail `Internal`; malformed imported programs fail `InvalidArgument`; incompatible contracts fail `TypeMismatch`; validation work or allocation exhaustion remains `ResourceExhausted`. A named planner's callback failures, exceptions and invalid generated programs identify that planner by name and version in the diagnostic.

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

GPU COPY reads one affine source and writes one dense destination, preserving sample bytes across all Host, Shared and Device domain combinations. Signed and zero source strides, unaligned byte origins and odd byte counts are supported. Metal uses a byte-addressed compute kernel; Vulkan coalesces contiguous suffix dimensions and encodes the remaining regions in bounded batches of buffer-copy commands.

For a destination of `B` bytes, a GPU COPY declares `B` bytes of Shared scratch per Host endpoint. A Host-to-Host GPU COPY therefore requires two disjoint segments totaling `2B`; Shared/Device endpoints require no staging. `OperationProgramBuilder::finish` derives this declaration during compilation. Custom physical programs and imported artifacts must already contain sufficient scratch. Preparation leaves the task graph and scratch declarations unchanged. Block admission reserves native staging capacity separately for each GPU lane. Host source gathering precedes native submission; a Host destination is copied from staging after native success and before terminal task completion. Cancellation never releases accepted work early.

External allocation contracts are selected by declaration name before compilation:

```cpp
ps::PlanningOptions options;
options.input_layouts["image"] = ps::physical::InputLayout::DenseHost;
```

Omitted declarations use `DirectTiles`, which borrows distinct compatible tile allocations. `DenseHost` describes row-major Host storage for a complete tensor member, including batch axes. Its raw tile views share the member strides and capacity, with offsets computed from their logical origins. `submit` checks those views relative to the bound base address; the owning storage may contain unused bytes before or after that range. Source owners remain immutable and retained through their last read. For a UInt8 `[4,4]` member with extent 2, raw tiles have strides `[4,1]`, offsets `0,2,8,10` and capacity 16. Four COPYs produce separate 4-byte tiles with strides `[2,1]`.

The planner creates one `Imported` value per used `(block, input)` and one COPY per selected tile, deduplicated across consumers in that block. `import_source` identifies the canonical Input, `owner` identifies the block, and `source_value` is zero. The validator proves that each import copies the same member, tile index, type and logical extent from that input. Payload consumers and terminators use imported values; schema-only reads may still reference the input. Edge arguments rename imported handles into the destination's dense arguments. A repeated block activation creates fresh import allocations.

A COPY serving an operation belongs to the earliest actual consuming call in that block. Its saved call attribution remains fixed when `select_outputs` removes other tasks. An input used only by the condition, edge arguments or RET requires a boundary COPY with `call = none`. Boundary COPYs issue after every real call retires successfully, use the same allocation/completion/hold protocol, and produce no CallRecord. This also covers input-only programs with zero calls. Their allocations count toward the full block reservation. Raw views, import provenance and COPY tasks are serialized and participate in Plan identity; preparation and submission perform no topology changes.

`OperationRegistry::prepare_operation` copies input schemas before invoking static callbacks and stores static parameters by value, preserving floating-point bit patterns. Static callbacks, lowering and kernel preparation share the captured schemas. Callers may edit their original schemas and parameter maps after preparation returns; those edits require a new preparation to affect execution. Caller mutation must not race preparation. Output selection and concurrent Runs retain the same immutable static state. A prepared kernel retains that state through its own release callback, even after the registry and external preparation handles are destroyed.

`OperationDefinition::validate_resources(state, resources)` checks selected immutable resources once per `PreparedOperation`, after static specialization and resource selection. It runs before publication of the preparation, including Empty calls and artifact rebinding. The callback borrows immutable static state and only declared frozen resources; it reads no sample payload and performs no I/O. Failure rejects preparation and releases temporary owners. Lowering and per-task preparation retain the accepted resource owners without repeating the check.

`prepare_kernel` receives the symbol, static operation metadata, complete task views, encoded task parameters, scratch bounds, resources and device. It verifies the symbol's invocation contract and produces an immutable `PreparedKernel` with direct entry points, resource owners and pipelines. PreparedPlan retains these contexts and their modules; each invocation borrows its context through completion. Context destruction occurs after all referencing Runs drain. Linking selects the factory by the saved `KernelDesc::abi_version`: version 8 uses `prepare_kernel`, while trusted C++ CPU version 9 uses `prepare_kernel_v9`. A missing corresponding factory rejects preparation; it does not downgrade or rerun lowering. FRQ-01..06 require typed CPU version 9, so their older version-8 Plans require recompilation. Module ABI 8, native service ABI 2 and Plan v5 serialization layouts remain unchanged.

`Placement` fixes backend class, numerical profile, memory domain and queue capability. `prepare` binds these to concrete device objects and kernels. It can reject an unavailable backend but cannot change task topology, implementation profile or COPY edges. `CpuExact` requires CPU tasks. The static planner's `NativeGpu` mode requests GPU lowering for GPU-capable operations and CPU lowering for CPU-only operations. A GPU lowering may explicitly combine CPU and GPU tasks within one call when the operation advertises both capabilities; every non-COPY task is checked against those traits. COPY placement follows its declared domains. One Plan can therefore contain both backends without a runtime choice. CPU/GPU alternatives for an operation require separately compiled Plans. No implicit retry or placement fallback occurs during preparation or execution.

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
4. Every task belongs to exactly one block. Operation tasks and their import COPYs belong to one real call; terminal input import COPYs have `call = none`. Call ordinals are contiguous. A producer's call ordinal is no greater than its consumer's; a real call cannot depend on a boundary COPY. Task array position imposes no execution order.
5. Each effectful call has one terminal effect task after all its pure preparation tasks. The effect has no outgoing edge to preparation in that call and is eligible only at retirement head.
6. Required conditions, returns, edge arguments and dominating live-through values exist on the executing path. Only a chosen successor receives a dynamic activation.
7. Resource vectors cover outputs, persistent state, descriptors, command metadata, queue/completion records, delivery records and scratch. The backend supports the declared synchronization.

These checks validate declarations, not arbitrary native implementation code. Writing every declared output byte before success and obeying numerical semantics are operator obligations. Undeclared accesses or invalid numerical preconditions violate the unchecked contract; diagnostics are not guaranteed.

`OperationProgramBuilder` enforces aggregate table-record and constructed-reference limits before appending entries. Shared tiles count once; each actual task read counts separately. Finalization checks derived predecessors, successors and live-ins against the same limits. Task parameter bytes remain subject to final work validation.

`select_outputs` closes a subset over existing tables, preserving control/effects and call order. It rebuilds task membership, adjacency, return holds and resource bounds. It neither calls a mapper nor adds missing tiles. A request outside compiled coverage requires compilation. An empty selection still executes control and effects.

## Run, control flow and return interface

`ExecutionPlan` shares serializable immutable program data across copies; output selection detaches its private storage before editing. `PreparedPlan` retains that body and owns linked kernels, device objects and module/resource leases. Preparation admits its new tables, query/bridge metadata and kernel wrappers under the context resource root. `RunHandle` owns mutable state and immutable binding leases, and retains its prepared program through drain. Concurrent Runs share prepared code and context pools with distinct output allocations.

The logical API is `prepare(plan, device_context)`, `submit(prepared, bindings, options)`, `request_cancel(run)` and `join(run)`. Each fallible operation returns typed status. `join` waits for all accepted submissions to quiesce, then returns the complete named Result set or one terminal failure. If copying a terminal failure message exhausts memory, `join` preserves the error code, reason and structured detail and returns an empty message. Result owners can outlive the Run and context by retaining their allocation release context.

An optional `on_call_retired(CallRecord)` receives sequence number, activation, source call, status and selected exported values. A healthy observer receives one record per retired call, including elided calls. Intermediate values omitted from export have no delivery hold. Records are stored in a bounded delivery queue reserved at block admission; user callbacks run outside the coordinator, independently of completion ingestion.

Each Run has one non-reentrant delivery lane. Callback n+1 starts only after callback n returns normally, so both invocation and normal-return order follow retirement order. Different Runs may deliver concurrently. `join` waits for task drain and delivery settlement; a callback must not synchronously join its own Run. A slow callback can delay join or consume capacity needed by the next block, but cannot stop CPU/GPU completion ingestion.

A callback exception closes that Run's delivery lane immediately: no later callback starts, queued records are discarded with their delivery holds released, and independently retained user values remain valid. Lane closure and enqueue are serialized. Enqueue to a closed lane rejects the record without taking its owners; the coordinator releases those delivery holds and marks the record settled, including retirement racing the failure notification. The delivery lane publishes one reserved settlement event that atomically carries `OperationFailed`, observer origin and the offending CallSeq. The failing callback record remains unsettled until the coordinator consumes that event; the coordinator records the abort before marking that record settled, in the same state transition. A success acknowledgment followed by a separate error event is forbidden. The coordinator stops further admission/retirement and drains accepted work. Retirement may already have enabled later effects before the callback failed; those effects cannot be retracted.

Final RET data remains internally held until all accepted work has quiesced and the coordinator has acknowledged settlement of every retired record, including the last callback. With no observer, record settlement is immediate. The coordinator consumes pending delivery/stop events and latches the terminal outcome at one serialized finalization point; only success transfers the final RET owners to the caller. An exception from the last callback therefore prevents successful RET even if all kernels have completed. Cancellation observed before that point retains its documented precedence; requests after it cannot alter the outcome. The prototype models owning retirement records directly and does not yet implement this callback protocol.

Block entry creates all task/call state for a fresh monotonic activation. A successful terminator runs after all block calls retire and all tasks complete. The coordinator reads the host-visible condition, captures every chosen edge argument and live-through value simultaneously, obtains next-frame holds, then releases the previous frame. Compiler liveness includes dominating values used beyond the immediate successor, even without explicit block arguments. Unchosen successors are not instantiated.

Loops create new materialized tile identities per activation. Incoming values retain their old allocation through their final read. Every block entry consumes bounded work, including empty loops, and rejects activation/sequence overflow. A device-resident branch scalar requires an explicit host COPY. Cancellation and currentness are checked at coordinator safe points.

Typed invocation-9 numerical checkpoints precharge work/stages through a borrowed Run service. Coordinator and kernel charges share atomic Run-then-Context-root admission; `issued_work` counts successful admissions, not CPU instructions or legacy v8 internal arithmetic. A real host budget rejection latches `ResourceExhausted` with `WorkLimit`/`StageLimit`, `Resource`/`Run` attribution and the known node/task. The coordinator observes this latch, aborts scheduling and drains accepted work even if the rejecting kernel later reports OK. Lane-safe-point checks bound further issue; a racing already-accepted invocation retains its owners through return. Cancellation preserves terminal priority, and admitted charges are not refunded. Per-invocation operator `max_work` failures remain ordinary kernel failures and do not impersonate host Run-budget failures.

Each produced dynamic Result reserves a process-unique ObjectId at block admission, before kernels issue. The move-only reservation uses the same allocator as `ResultBuilder` and can construct one canonical Result object. All tiles and members of that value expose the same ID through `ps_kernel_invocation_v8::input_object_ids`, in input-view order. Fresh loop activations and independent Runs reserve fresh IDs; CFG arguments and live-through aliases retain their source association. The ID is runtime metadata and is absent from Plan serialization and content digests.

The canonical Result contains the original dynamic value's complete compiled tile set. A complete retirement export, complete RET selection and complete CFG alias share that object when materialized. A partial selection creates a fresh Result object, including when a block argument contains every tile requested downstream but fewer than its original producer. External direct bindings reuse their captured Result only when selected tiles cover its entire captured footprint. Identity state retains only a weak Result reference; a boundary holds a strong canonical owner only when a successor needs the original complete set, allowing discarded tiles of partial aliases to be released.

DenseHost imports have two explicit identities. Kernel input views retain the external source ObjectId for association checks. The copied storage has a fresh reserved ObjectId when exported as a Result; its complete aliases share that COPY object. Passing the exported copy into a later Run therefore uses the copy's ObjectId. An explicit operation that produces a copied value also receives a fresh identity. Equal bytes alone do not establish ObjectId equality.

Named RET values publish only after successful execution, delivery settlement and the final stop/currentness check. Already retired call results remain valid if a later call fails. Source-bound Plans retain graph-currentness semantics; standalone imported Plans are source independent. A host/Demand facade owns binding replacement: it invalidates an obsolete Run's generation and requests abort, then starts a new Run. Active tasks keep their immutable binding snapshot and owners through drain.

## Compiler artifacts

The artifact headers are `psir 5` and `psxp 5`. IR changes because call order means retirement and effects are explicit. Plan stores all physical tables, unchecked semantic mode, source identity, kernel contract versions, output selections and resource bounds. Canonical field encoding is independent of C++ padding. It contains no process pointers, native handles, Run state or cache contents.

Import performs bounded parsing, structural validation, symbol/resource linking and immutable publication. It runs no frontend, optimizer, mapper or planner. Import restores only static Plan data and resolves static contract/resource identities. Device object construction occurs exclusively in the subsequent explicit prepare stage. Digests identify content, not trust in native code. New IR, Plan and operation-manifest identity domains are required. v4 artifacts are rejected with a recompile diagnostic; there is no compatibility translation.

| Previous boundary | Implemented replacement | Migration |
| --- | --- | --- |
| IR order | Ordered retirement and dependency-driven issue | IR semantics, optimizer legality and identity |
| PlanStep/PlanTileInstruction | Task/Tile/Call/Block tables | Builder, validation, selection and codecs |
| Continuation ABI 7 | Fixed invocation and asynchronous backend completion | ABI 8 and traits 30; rebuild plugins, no continuation shim |
| Need/Publication/certification | Static reads and preallocated writes | Remove polling, grant assertions and automatic numerical certification |
| Shared stages/checkpoints/blocks | SSA intermediate tasks and scratch | Remove executor checkpoint/block/result reuse |
| Residency/fallback | One domain per tile, explicit COPY | Remove implicit mirror caches and automatic retry |
| Prefix observer | Ordered CallRecord and final RET | Replace per-tile prefix notification |
| Package/data schema | Independent version axes | Package 0.39.0 / SOVERSION 0.39; schema encoding 6 retains structural publication metadata |

The current implementation removes the execution-specific continuation, payload-cache, implicit-residency, storage-reuse and Need/certification paths described by the historical ADRs. ADR 0016 now documents immutable Run inputs; ADR 0018, ADR 0019, ADR 0021, ADR 0022 and ADR 0023 identify their superseded execution portions. Compatible input ownership, fixed-grid geometry and product boundaries remain. Current architecture pages describe the physical implementation. Existing ADR acceptance does not change this proposal's formal status.

## Prototype and implementation acceptance

The C++ physical data layer is declared in [`physical_program.hpp`](../../include/photospider/compiler/physical_program.hpp). `physical::finalize_program` derives adjacency, live-through sets and allocation bounds; `physical::validate_program` independently checks the complete tables, including block-argument physical compatibility. The internal `TaskSchedule` owns block-local readiness, submission identities, failure cuts and retirement. `CpuWorkers` executes fixed invocation/completion pairs on a bounded worker pool and drains accepted work during shutdown. `BlockMemory` reserves the complete output bound across Host, Shared and Device domains, per-lane scratch and storage metadata before payload allocation. Physical storage retires at its last use while the block protects the reserved capacity through its boundary; surviving exports then retain only their own allocation charges. `test_physical_cpu_schedule` exercises these components together with separate payload allocations and real CPU overlap. The public compiler emits physical tables through `OperationProgram` lowering. `CompilerArtifact` encodes v5 tables and validates imported adjacency, liveness, reservations and IR bindings without rerunning lowering. Fixed invocation ABI 8 provides direct CPU entries and an asynchronous GPU submission contract. Trusted C++ CPU operations can explicitly use independent typed invocation/preparation ABI 9 with a Run work/stage service; this does not change version-8 records or the module-8 transport. `KernelPreparation` carries the actual task read/write layouts, ports, parameter bytes and scratch bounds; each kernel checks its invocation contract before returning an unchecked entry point. COPY-only definitions do not require a kernel preparation callback. The public `ExecutionContext::prepare` links fixed CPU kernels, Host-domain asynchronous GPU entry points and native Metal/Vulkan kernels over Shared and Device storage; `submit` captures bindings and starts a coordinator; `RunHandle::join` waits for accepted tasks and observer delivery. `test_fixed_execution` exercises concurrent issue, ordered retirement, observer failure, cancellation drain and context-independent Results. `test_fixed_cfg` exercises loops, simultaneous edge arguments and immutable input bindings. Direct bindings require separate compatible tile allocations. DenseHost input lowering emits explicit CPU COPYs, including zero-call boundaries. Bundled operations use fixed CPU lowering/preparation/invocation, or explicit COPY for identity. Registration does not promise every profile or layout mode: ISA-dependent profiles can be unavailable, and nonempty format `layout=view` requests are rejected where unique materialization is required. The Metal adapter prepares fixed MSL pipelines and uses command-buffer completion to settle accepted work. Vulkan prepares fixed SPIR-V pipelines and immutable uniform buffers; a device completion worker drains submission fences before notifying the coordinator.

Run `python3 docs/kernel-architecture/prototypes/ooo_executor.py` for the semantic model and add `--test` for its behavioral checks. Logical event durations and distinct byte buffers exercise explicit/read-derived dependencies, failure cuts and task-error attribution, separate retirement/RET holds and retirement. It models one block, with a separate simultaneous-edge-transfer helper. It does not execute a complete CFG, implement delivery callbacks, model resource vectors/metadata capacity, or provide native backends, a public artifact parser or performance measurements.

Native allocation preparation resolves each Shared and Device output and scratch capacity against the concrete device without changing the Plan. Block admission reserves Host, Device, Shared and Payload capacity together, including device rounding. Each allocator draws payload slices from that reservation; a changed native capacity or unsatisfied alignment fails the block before issue. Exported storage retains its own slice after the block and device wrapper retire. `test_native_block_memory` exercises native allocations, capacity rejection, mixed-domain Result coverage and shader execution through Shared and Device bindings.

`PlanningOptions::input_domains` fixes external input residency, with Host as the default; `DenseHost` accepts only Host. CFG arguments inherit the domain of their incoming allocation, and incompatible joins require an explicit COPY. Results retain separate CPU and Device fragments under one descriptor coverage and ObjectId. CPU windows intersecting Device fragments return `BackendUnavailable`; internal Device acquisition preserves captured descriptor authority and retains only the selected allocation. `test_device_result` executes native Device producers through public prepare/submit/join, ordered call exports, same-device input reuse and foreign-device rejection. `test_native_copy` covers all nine domain pairs, affine byte layouts, 1/2/4/8-byte samples, staging rejection, artifact import, concurrent Runs and final resource release.

The coordinator arms each GPU completion slot before calling `submit_gpu`. `maximum_gpu_submissions` limits accepted work across the context; each Run reserves its completion records and the per-domain scratch maximum for its GPU lanes before block issue. A rejected submission settles locally. Accepted work retains invocation views, scratch and kernel owners until a matching terminal completion, including cancellation, shutdown and malformed acceptance. An incorrect completion key aborts the Run but cannot settle the real submission. Completion may occur inline. `test_fixed_async` uses a controlled Host-domain fixture to verify these ownership and credit rules; it provides no native-device evidence.

Native kernel service ABI 2 validates shader bindings and copies fixed dispatch metadata during preparation. Prepared kernels retain pipelines across cache eviction. A native GPU Plan can combine CPU-only numerical calls, GPU numerical calls and explicit COPY tasks. Host/Shared COPY may use CPU placement; a Device endpoint requires GPU COPY. `test_fixed_native` executes real Metal shaders through a C11 module, covering tile dependencies, independent calls, artifact import, partial output selection and concurrent Runs with different external bindings. Device command failure aborts the Run while accepted submissions drain. `test_vulkan_gpu` exercises the Vulkan fixed path, explicit COPY, distinct intermediate outputs, concurrent submissions and completion-thread Device destruction. Real device-loss injection is not covered by these tests.

Focused validation covers compiler-to-Plan-to-CPU overlap, malformed artifacts, selection/loops, asynchronous native completion, explicit transfers, cancellation/failure owner drain, ordered observer delivery and resource accounting after export release. Terminal-priority regressions hold accepted work while testing both device-first and observer-first aborts. Static mixed CPU/GPU lowering is checked both across calls and within one call. [Native measurements](Parallel-Execution-Model.md#native-measurements) record five Runs per workload on Metal and Vulkan, full-block capacity rejection, COPY, drain and independently correlated device timing. Physical device-loss injection and optional multi-task packets are not claimed by this baseline. Installed-consumer testing remains suspended during kernel development.

## Limitations

Shapes and task graphs per block are fixed. Branch prediction, cross-iteration speculation, runtime dependency discovery, fixed-size task ROBs, payload cache hierarchies, automatic recomputation, live tile aliasing and implicit migration are excluded. A callback that never terminates or a device that cannot establish quiescence can delay drain indefinitely; timeout never authorizes early buffer release.
