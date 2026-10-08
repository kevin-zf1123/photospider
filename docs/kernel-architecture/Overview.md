# Architecture Overview

## 1. Core summary (TL;DR)

Photospider is an embeddable C++17 kernel that validates local workflows, compiles immutable plans, and executes them against caller-supplied Result bindings. A `GraphContext` owns source revisions; an `ExecutionContext` owns bounded workers, operation definitions, optional cache retention, and resource accounting. The daemon consumes the installed kernel API and owns its own sessions and process lifecycle.

## 2. Mental model and intuition

```text
WorkflowDocument -> GraphContext -> GraphSnapshot
                                    |
                                    v
                         analyze -> SemanticGraphIR
                                    |
                                    v
                         optimize -> OptimizedGraphIR
                                    |
                                    v
                                  Plan
                                   |
ExecutionBindings ----------------> Run
                                   |
                        +----------+----------+
                        |                     |
                 ready CPU steps       ready native steps
                  compete for a         enter one configured
                 bounded CPU pool          GPU lane
                        +----------+----------+
                                   |
                       named Result outputs
                           + diagnostics
```

The compiler owns static validation and planning. Each execution owns its ready work, Result producers, cancellation state, dependency recorder, and diagnostics. Runs share the context's worker pools and resource root, while returned Results keep their backing and leases alive independently.

## 3. Contracts and interfaces

```cpp
class Compiler {
 public:
  Result<SemanticGraphIR> analyze(const GraphSnapshot&,
                                  ResourceBindings = {}) const;
  Result<OptimizedGraphIR> optimize(const SemanticGraphIR&) const;
  Result<ExecutionPlan> plan(const OptimizedGraphIR&,
                             const PlanningOptions& = {}) const;
};

class ExecutionContext {
 public:
  Result<ExecutionResult> execute(
      const ExecutionPlan&, ExecutionBindings = {},
      const CancellationToken& = CancellationToken(),
      const ExecutionOptions& = {});
};
```

These are abbreviated public signatures; unrelated members are omitted. Each stage returns a complete immutable value or a typed failure. Planning retains all static metadata needed for schema validation and identity, then derives executable demand from the selected output's input projection. Execution checks plan currentness and the frozen operation-registry identity before callbacks, then validates Result schemas, requested coverage, dependency evidence, and publication finality. `analyze` accepts immutable `ResourceBindings` for static facets such as color profiles.

`CpuExact` is the default execution mode. `NativeGpu` permits placement only for operations that declare a native implementation and match the configured backend. CPU work uses the fixed context pool; native callbacks use the configured backend lane. Cancellation is cooperative, and callbacks already entered may finish before the Run returns.

## 4. Non-goals and boundaries

- The kernel does not own daemon sessions, IPC, persistent jobs, or process lifecycle.
- Compiler IR and plans are in-process values, not a serialized workflow format.
- Native operation modules are trusted in-process code; ABI validation is not sandboxing.
- GPU availability does not imply that every operation has a native implementation.
- Results are in-memory owners, not durable identities or recovery records.

## 5. Consequences

Planning can be reused with new bindings, but runtime input addresses are not part of a plan. Queue admission and resource limits can reject a Run when configured bounds are exhausted; callers should use the returned status as the authoritative outcome. Retaining a Result retains its storage lease, so callers control how long that capacity remains charged. Native placement, fallback, transfers, and optional completed-result retention can change the work and memory cost of a Run. Block-state cache-hit counts are implementation observations, not performance guarantees.
