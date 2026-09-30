# ADR 0005: Graph Contexts Publish Source Before Compilation

- Status: Accepted

## 1. Core Summary (TL;DR)
`GraphContext` stores an immutable copy of a versioned `WorkflowDocument` and publishes replacements with a new revision. `Compiler` validates snapshots into complete immutable stages. Source publication and semantic validity remain separate so invalid input never leaks partial compiler output.

## 2. Mental Model & Intuition

```text
caller document --> GraphContext --snapshot/revision--> analyze --> optimize --> plan
                         |                                  |           |         |
                         +-- replace: revision advances ----+-----------+---------+
                                                            stale stages rejected
```

The context serializes snapshot capture and replacement. Compiler stages check the captured revision before publication; replacing source makes prior stages stale.

## 3. Formal Contracts & APIs

```cpp
struct WorkflowDocument { std::uint32_t schema_version = 3; /* inputs, nodes, outputs */ };
class GraphContext {
 public:
  explicit GraphContext(WorkflowDocument);
  GraphSnapshot snapshot() const;
  std::uint64_t replace(WorkflowDocument);
};
class Compiler {
 public:
  Result<SemanticGraphIR> analyze(const GraphSnapshot&, ResourceBindings = {}) const;
  Result<OptimizedGraphIR> optimize(const SemanticGraphIR&) const;
  Result<ExecutionPlan> plan(const OptimizedGraphIR&, const PlanningOptions& = {}) const;
};
```

The context copies caller data. `snapshot()` returns a coherent document/revision pair. `replace()` allocates the new immutable document before taking the mutex, then publishes the document and next revision together under the lock. Allocation failure leaves source and revision unchanged; revision overflow throws before publication. Context destruction sets the shared revision token to zero, invalidating outstanding snapshots.

Analysis validates bounded document and text input, unique IDs, references, ports, operation and required typed-parameter contracts, acyclic topology, and inferred output type/shape/Region metadata. It checks snapshot currentness before publishing a complete `SemanticGraphIR`. Optimization and planning likewise return complete stages or typed failures; allocation failure may propagate as `std::bad_alloc`. See [Graph lifecycle](../kernel-architecture/Graph-Lifecycle.md) and [Compiler and execution](../kernel-architecture/Compiler-and-Execution.md) for current validation details.

## 4. Non-Goals & Explicit Boundaries
- `WorkflowDocument` is compiler input. Parsing, filesystem access, storage, and daemon error mapping belong to consumers.
- Replacing source does not imply successful validation or execution.
- Snapshots do not keep a destroyed context current; context destruction invalidates them.

## 5. Consequences
Callers can atomically replace source without mutating existing snapshots. They must recompile after replacement and handle `Stale` results when replacement races a pipeline. Invalid graphs consume validation work but publish no partial IR or plan.
