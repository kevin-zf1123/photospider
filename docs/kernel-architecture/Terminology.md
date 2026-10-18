# Kernel Terminology

| Term | Meaning |
| --- | --- |
| `WorkflowDocument` | Caller-owned source graph used as compiler input. |
| `GraphContext` | Owner of a copied workflow document and its current revision. |
| `GraphSnapshot` | Coherent source and revision capture used by compilation. |
| `IRProgram` | Editable frontend-neutral SSA program: input declarations and bindings, unique call declarations, blocks, an entry block, an ordered return signature and an `IRForm`. Accepted by `Compiler::bind_ir`. |
| `ValidatedIR` | Immutable validated, prepared IR; frontend, import and optional optimizers publish this type. Both forms can be planned directly. |
| `IRForm::DependencyPaths` | Raw IR form retaining repeated Call ID requests in ordered paths. The same ID denotes one invocation and its SSA results, including effects. `analyze` emits complete DAG path expansion; optimization removes duplicate requests. |
| `IRForm::ControlFlow` | Executable SSA control flow with one static site per call. Branches and loops use explicit terminators and block parameters. |
| dependency chain | A raw path segment split at upstream branch switches, multi-input joins and new roots. Shared ancestors may occur in several chains; chain boundaries do not require graph nodes to have no fan-out. |
| SSA value | `IRValueId`-named Result defined exactly once by a call output, a block parameter or an external input binding. Every use must be dominated by its definition. |
| basic block / CFG | `IRBlock`: an ordered call list ending in one terminator. In ControlFlow, blocks and edges form the executable CFG and each call has one site. In DependencyPaths, blocks form a linear, parameterless sequence of request chains with a final return. |
| block parameter | Typed value assigned when an edge enters a block; all arguments of one edge are captured before any parameter changes. Loops carry values through block parameters. |
| conditional jump | Branch on a Result with exactly one `UInt8` sample: zero selects `if_false`, any other value `if_true`; only the chosen successor runs. |
| `ExecutionPlan` | Validated local program: `steps()` declares every call output with its backend and fixed grids, and `blocks()` holds the tile instructions and terminators that execution follows. |
| plan step | `PlanStep`: declaration of one call output. A step can have no instructions; the number of steps is not an execution count. |
| tile instruction | `PlanTileInstruction`: one output tile or shared-stage target with its publication target, fixed input and shared grants, block-local dependencies and compiled CPU fallback flag. |
| static mapping | `OperationDefinition::map_dependencies` (C: `map_dependencies` hook): pure compile-time callback that returns the tiles one target may read. `complete_input_dependencies` covers projected ordinary inputs; shared-stage reads require explicit mapping entries. |
| fixed grant | `PlanTensorRead` or `PlanSharedRead` stored in an instruction; the only data that instruction may read at run time. A descriptor-only grant exposes schema facts and no payload. |
| synthetic return record | Dependency record of a named return at index `steps().size()` plus the return ordinal, returned by `ExecutionPlan::output(name).record_index`. |
| compiler artifact | Version-4 text: `.psir` stores either validated IR form and provenance; `.psxp` stores the complete physical plan. Earlier versions require re-export. |
| artifact manifest | Operation keys with contract digests and resource identities listed in an artifact, readable through `CompilerArtifact::inspect` without a registry. |
| `ExecutionContext` | Owner of bounded workers, frozen operation definitions, caches, and resource accounting. |
| `ExecutionRun` | Private state for one synchronous execution call: program counter, block values, bound grants, evidence and diagnostics. |
| static VM | The interpreter inside a Run. It executes one plan instruction at a time in stored order and follows the CFG; it never maps dependencies, creates producer tasks or reorders instructions. |
| `ExecutionBindings` | Exact-name per-run input owners, each an owning `ResultRef` checked against its declaration schema. |
| `ResultRef` | Owns a static Tensor-only Result schema, captured coverage, backing owners and ObjectId provenance. |
| `Tensor` | A descriptor, exact authorized Footprint and owned TensorSpan fragments; the single numerical carrier. |
| operation metadata | `OperationMetadata`: the complete Result schema at an operation boundary. Specialization infers output structure from input structure and static parameters. |
| `metadata_owner` | Resource lease that pays for a schema's managed nested containers; budget ownership, not interpretation. |
| `ImmutableResource` / `ResourceSet` | Content-addressed immutable bytes (length and SHA-256) and an immutable set of them. Operations select resources by identity at preparation; plans retain them. |
| validator | Registered `ValidationDefinition` bound explicitly to a port by key, version, and implementation. It checks host-authorized Tensor samples. |
| diagnostic report | Fixed-size opaque record type registered as `DiagnosticDefinition` and merged by `DiagnosticAccumulator`. |
| `DomainErrorCode` | Opaque 64-bit failure code that the kernel stores in `FailureDetail::domain_code` without interpretation. |
| operations SDK | `Photospider::ops_sdk`, headers under `include/photospider/ops/`: color descriptions, domain codecs and arithmetic, description editing, LUT bake contract, quality and numeric reports, Layer types. Not part of the kernel. |
| `Region` | Logical subset in descriptor-axis coordinates; it does not describe bytes. |
| `TensorSpan` | One owned rectangular mapping inside Tensor; actual signed strides and backing ownership. |
| operation traits | Frozen metadata that declares input/output contracts, resource bounds, and backend capabilities. |
| operation registry | Frozen set of trusted operation definitions used to compile and execute plans. |
| digest / cache key | Non-security identity used for reproducibility checks or disposable cache lookup. Compiler digests include program identifiers; content cache keys do not. |
| cancellation | Cooperative request that prevents successful late result publication. |
| CPU fallback | A new CPU attempt with the same grants after a GPU callback reports backend unavailability before publishing output or reading input, allowed only where the planner set the instruction's `cpu_fallback` flag. |
| canonical image | An operation-domain convention: ordinary image operations default to straight alpha and host backing defaults to tiled planar. No Tensor conformance label or automatic validator is required; explicit association conversions remain available. |
| boundary payload | Premultiplied numeric tensor produced or consumed by FMT-04 adapters for export or external interfaces. The association is stated by the adapter's explicit parameters; the tensor itself carries no label, so a consumer relies on its own explicit parameters or bound validator. |

`SessionId`, `JobId`, daemon IPC, and process lifecycle belong to `photospider-daemon`; its Jobs are ephemeral. Persistent results and recovery are outside the current product boundary. Kernel results remain in-memory values whose backing lifetime follows their owners.
