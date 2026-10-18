# Architecture Overview

## 1. Core summary (TL;DR)

Photospider is an embeddable C++17 kernel that validates local workflows, compiles them through independent IR and plan stages into a fixed tile instruction program, and executes that program against caller-supplied Result bindings. A `GraphContext` owns source revisions; an `ExecutionContext` owns bounded workers, operation definitions, optional cache retention, and resource accounting. The daemon consumes the installed kernel API and owns its own sessions and process lifecycle.

## 2. Mental model and intuition

```text
WorkflowDocument -> GraphContext -> GraphSnapshot      IRProgram (other frontend)
                                    |                         |
                                    v                         v
                         analyze -----------------> ValidatedIR <---- .psir
                                                          |
                                                          v
                                     optional optimize + validate -> ValidatedIR <---- .psir
                                                          |
                                                          v
                                   plan: static mapping, tile expansion,
                                   fixed grants, CFG of instruction blocks
                                                          |
                                                          v
                                                    ExecutionPlan <---- .psxp
                                                          |
ExecutionBindings --------------------------------------> Run
                                                          |
                                     sequential VM: pc -> block -> instructions
                                                          |
                                  +-----------------------+---------------------+
                                  |                                             |
                     CPU callback on the context pool            native callback on the
                     (range or tile service inside it)           configured GPU lane
                                  +-----------------------+---------------------+
                                                          |
                                         named Result outputs + diagnostics
```

The compiler owns static validation, dependency mapping and planning. Each Run owns its program counter, block values, bound grants, Result producers, cancellation state, dependency recorder, and diagnostics; it executes one plan instruction at a time. Parallelism comes from separate Runs sharing the context's pools and from CPU range, CPU tile and GPU work inside one operation callback. Returned Results keep their backing and leases alive independently.

Planner accepts the initial ValidatedIR directly; the optimizer edge is optional.

## 3. Contracts and interfaces

```cpp
class Compiler {
 public:
  Result<ValidatedIR> analyze(const GraphSnapshot&,
                                  ResourceSet = {}) const;
  Result<ValidatedIR> bind_ir(IRProgram, ResourceSet = {}) const;
  Result<ValidatedIR> optimize(const ValidatedIR&,
                               const OptimizerInvocation& = {"optimize", {}}) const;
  Result<ExecutionPlan> plan(const ValidatedIR&,
                             const PlanningOptions& = {}) const;
};

class CompilerArtifact {
 public:
  static Status save_plan(const ExecutionPlan&, const std::string& path);
  static Result<ExecutionPlan> load_plan(std::shared_ptr<OperationRegistry>,
                                         const std::string& path,
                                         ResourceSet = {});
};

class ExecutionContext {
 public:
  Result<ExecutionResult> execute(
      const ExecutionPlan&, ExecutionBindings = {},
      const CancellationToken& = CancellationToken(),
      const ExecutionOptions& = {});
};
```

These are abbreviated public signatures; unrelated members are omitted. Each stage returns a complete immutable value or a typed failure. `analyze` expands the DAG from named outputs into `DependencyPaths`, preserving shared ancestors in every referencing path with the same Call and SSA IDs. `optimize` keeps the first request for each call, removes empty chains and connects the retained chains as executable `ControlFlow`. For example, raw `ABG / AB / AC / DE / F` becomes `ABG / C / DE / F`. `bind_ir` also accepts explicit ControlFlow, including loops and conditional branches on one `UInt8` sample; that form is preserved by optimization.

`plan` calls each operation's static mapper, expands the reachable output tiles into deduplicated instructions with fixed read grants, and validates the result. Its tile deduplication tracks work already scheduled in the plan independently of runtime caches. Execution checks plan currentness and the frozen operation-registry identity before callbacks, then follows the instructions and CFG. Each call's produced outputs are certified before the next call starts. `analyze` accepts an immutable `ResourceSet` of content-addressed resources; each operation's preparation selects the identities it needs, such as an ICC profile named by an explicit color parameter. Result and Tensor carry structure and data only; domain interpretation is an explicit operation parameter, and the kernel performs no label matching on connections.

`CompilerArtifact` saves and loads v4 text (`.psir` for unified IR, `.psxp` for complete Plans). Raw and transformed IR may use dependency paths or CFG; Plans save their privately lowered CFG. v1–v3 require re-export. Loaded values are independent of the original GraphContext but still require matching frozen registry/resources. Plan import invokes no optimizer, mapper or planner. See [Compiler and execution](Compiler-and-Execution.md#compiler-artifacts).

`CpuExact` is the default execution mode. `NativeGpu` permits placement only for operations that declare a native implementation and match the configured backend. CPU work uses the fixed context pool; native callbacks use the configured backend lane. Cancellation is cooperative, and callbacks already entered may finish before the Run returns.

## 4. Non-goals and boundaries

- The kernel does not own daemon sessions, IPC, persistent jobs, or process lifecycle.
- The kernel ships only the WorkflowDocument DAG frontend. There is no Python or text-language frontend, machine-code backend, JIT, or automatic library discovery from artifacts.
- The optimizer resolves repeated dependency-path requests while retaining every unique call; it preserves explicit ControlFlow input. Tensor shapes and tile grids are fixed at compile time; control flow does not provide dynamic shapes.
- Compiler artifacts are editable, versioned text files without signatures; their digests are reproducibility aids.
- Native operation modules are trusted in-process code; ABI validation is not sandboxing.
- GPU availability does not imply that every operation has a native implementation.
- Results are in-memory owners, not durable identities or recovery records.

## 5. Consequences

Complete dependency-path expansion can grow exponentially with DAG sharing. The frontend limits raw IR to 1,048,576 call occurrences and 65,536 chains, returning `ResourceExhausted` when expansion exceeds either bound. Fewer executable blocks reduce block transitions, but shared-stage state remains alive until the next block activation; block compaction alone does not guarantee lower memory use.

Plans can be reused with new bindings, saved and reloaded, but runtime input addresses are not part of a plan. Planning cost and plan size grow with the number of reachable output tiles; `PlanningOptions` bounds instructions and dependency edges, and a very large logical output should be planned with a sparse request rather than filtered afterward. Queue admission and resource limits can reject a Run when configured bounds are exhausted; callers should use the returned status as the authoritative outcome. Retaining a Result retains its storage lease, so callers control how long that capacity remains charged. Native placement, compiled fallback, transfers, and optional completed-result retention can change the work and memory cost of a Run. Block-state cache-hit counts are implementation observations, not performance guarantees.
