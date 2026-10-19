# Kernel Terminology

The execution terms below follow the [proposed breaking design](Compiler-and-Execution.md). They describe the replacement contract, not the current C++ class layout. [Chinese reader version](zh/Terminology.zh.md).

| Term | Meaning |
| --- | --- |
| WorkflowDocument | Caller-owned source graph with named inputs and outputs. |
| GraphContext / GraphSnapshot | Source revision owner and coherent compilation snapshot. |
| IRProgram / ValidatedIR | Editable SSA program and its validated immutable form. |
| DependencyPaths | Raw DAG call requests; repeated Call IDs identify one invocation. Planning deduplicates requests and coalesces eligible path blocks. |
| ControlFlow / CFG | Blocks with ordered call sites, typed block arguments and Jump/Branch/Return terminators. |
| SSA Result value | A call result, external input or block parameter with one static definition and dominated uses. |
| ExecutionPlan | Immutable serializable Task/Tile/Call/Block tables, constants, dependencies, placement requirements and resource bounds. |
| PreparedPlan | Device-bound immutable kernels, decoded layouts, direct entry points and resource/module owners retained across Runs. |
| PreparedKernel | Immutable operation context produced by device prepare; shared invocations may read it concurrently. |
| Run / RunHandle | One mutable execution with fixed bindings, dynamic activations, completion records and result owners. |
| block activation | One actual entry into a static block. Re-entering a loop creates a fresh activation. |
| Task | Scheduling unit: kernel, explicit COPY or terminal effect. It may produce several independent tiles. |
| TileId | Static tile descriptor index; `(RunId, ActivationId, TileId)` identifies a materialized runtime tile. |
| CallGroup / CallDesc | One IR call's selected tasks, return declarations and retirement ordinal, including a zero-task elided call. |
| predecessor / successor | Deduplicated producer-task readiness edge and its transpose. |
| pending | Number of distinct predecessors that have not completed successfully. |
| fixed read / view | Compiler-resolved input port, tile, logical coordinates, strides and byte bounds. |
| schema-only read | Read of immutable prepared schema facts without a payload dependency. |
| ready | Dependencies succeeded and the task can seek resources; resource credit may still delay issue. |
| issue / submission | Transfer of a prepared invocation and its owners to a worker/backend. |
| completion | All task accesses ended and successful writes have required visibility; enables internal consumers. |
| retirement | Ordered handoff of one call's status and selected exported values to the delivery stream. |
| release | Last hold ended; the allocation can be freed independently of retirement order. |
| failure cut | Smallest observed failed call ordinal; older necessary work continues, unsubmitted work at/after it is skipped. |
| abort | Run-wide admission stop followed by drain, used for cancellation, staleness and context failure. |
| Effect | One terminal external action in a call, issued only at retirement head after its preparation succeeds. |
| COPY | Explicit transfer/materialization that produces a new tile allocation in a declared domain. |
| packet / submission unit | Optional same-call GPU task group with one submission credit/completion event and a complete resource bound. |
| SubmissionId | Identity of an accepted task/packet submission, mapped to members; distinct from TaskId and CallId. |
| queue timepoint | Backend completion coordinate ordered by actual queue submissions, independent of call retirement ordinals. |
| block argument | A new name/owner for an existing immutable value; all sources are captured before destinations change. |
| live-through hold | Ownership preserving a value for a later block, including dominating values not passed as explicit arguments. |
| Result / ResultRef | Static schema plus selected tile handles and owning leases; no automatic dense output assembly. |
| Tensor / Region | Numeric storage descriptor and logical coordinate region; logical coverage is separate from physical strides. |
| ExecutionBindings | Exact-name immutable input owners and structural schemas fixed per Run. |
| unchecked operator contract | Numerical validity, declared access and truthful completion are implementation preconditions; the executor performs no sample certification. |
| admission | Atomic acquisition of the block's output/state/queue/completion/delivery/scratch capacity before issue. |
| compiler artifact | Versioned canonical IR/Plan content, without native pointers or mutable Run state. |
| digest | Non-security content identity for reproducibility, not native-code authentication. |
| operation registry | Frozen catalog of trusted implementations used for compilation and linking. |
| ImmutableResource / ResourceSet | Content-addressed immutable preparation inputs retained by prepared owners. |
| operations SDK | Domain algorithms/codecs and numerical semantics outside the generic scheduler. |

`SessionId`, `JobId`, daemon IPC and process lifecycle belong to `photospider-daemon`. Persistent results and recovery remain outside the product boundary. The replacement executor contains no payload cache hierarchy, implicit CPU fallback or runtime Need protocol.
