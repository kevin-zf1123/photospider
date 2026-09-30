# Architecture Overview

## 1. Core summary (TL;DR)

Photospider is an embeddable C++17 kernel that validates local workflows, compiles immutable plans, and executes them against caller-supplied bindings. A `GraphContext` owns source revisions; an `ExecutionContext` owns bounded workers, operation definitions, caches, and resource accounting. The daemon consumes the installed kernel API and owns its own sessions and process lifecycle.

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
                    named Values / PlanarImages
                           + diagnostics
```

The compiler owns validation and planning. Each `ExecutionRun` owns its ready work, intermediate Values, cancellation observations, and diagnostics. Runs share the context's worker pools and byte ledger, while returned Values keep their storage leases alive independently.

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

These are abbreviated declarations of public methods; unrelated members are omitted. Each stage returns a complete immutable value or a typed failure. The current optimizer copies semantic nodes into a distinct stage and computes a separate digest. Planning records named output demands and derives per-step input demands for Whole, Elementwise, and clipped Halo rules. Execution checks plan currentness and the frozen operation-registry identity before callbacks, then validates output descriptors and demanded coverage before publication. `analyze` accepts immutable `ResourceBindings` for static facets such as color profiles.

`CpuExact` is the default execution mode. `NativeGpu` permits placement only for operations that declare a native implementation and match the configured backend. CPU work uses the fixed context pool; native callbacks use the configured backend lane. Cancellation is cooperative, and callbacks already entered may finish before the Run returns.

## 4. Non-goals and boundaries

- The kernel does not own daemon sessions, IPC, persistent jobs, or process lifecycle.
- Compiler IR and plans are in-process values, not a serialized workflow format.
- Native operation modules are trusted in-process code; ABI validation is not sandboxing.
- GPU availability does not imply that every operation has a native implementation.
- Results are in-memory owners, not durable identities or recovery records.

## 5. Consequences

Planning can be reused with new bindings, but runtime input addresses are not part of a plan. Queue admission and the shared byte ledger can reject a Run when configured bounds are exhausted; callers should treat returned status codes as the authoritative outcome. Retaining a result retains its storage lease, so callers control how long result memory remains charged. Native placement, fallback, transfers, and cache reuse are visible through diagnostics and can change the work and memory cost of a Run.
