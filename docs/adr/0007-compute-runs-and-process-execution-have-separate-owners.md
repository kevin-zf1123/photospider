# ADR 0007: Execution Runs and Context Resources Have Separate Owners

- Status: Accepted

## 1. Core Summary (TL;DR)
A graph context owns source revisions; an execution context owns local resources; each execution call owns its run state and staged results. This separation permits graph replacement and shared worker use without retaining or publishing stale results.

## 2. Mental Model & Intuition

```text
GraphContext --> immutable snapshot --> compiler plan --+
                                                        +--> Run A --+--> caller results
ExecutionContext --> shared bounded CPU/GPU resources --+            |
                                                        +--> Run B --+ (separate state)
```

Runs compete for shared context capacity while retaining independent cancellation, bindings, intermediate values, and diagnostics. A run joins work only through explicit context-managed demand mechanisms.

## 3. Formal Contracts & APIs

```cpp
class GraphContext { public: GraphSnapshot snapshot() const; std::uint64_t replace(WorkflowDocument); };
class ExecutionContext {
 public:
  Result<ExecutionResult> execute(const ExecutionPlan&, ExecutionBindings = {},
      const CancellationToken& = {}, const ExecutionOptions& = {});
};
```

The plan captures graph currentness and frozen operation-registry identity. A call owns its bindings snapshot and run state; output values are caller-owned and may outlive the context. When a worker removes a queued attempt, it checks cancellation and plan currentness before transfer, resource admission, or operation entry. A stop observed after this cutoff may race with an in-process callback; callbacks drain cooperatively, and late completion cannot release dependent work or publish a result. Final publication checks cancellation and plan currentness again. See [Compiler and execution](../kernel-architecture/Compiler-and-Execution.md) for the full ordering.

Failures discard collected staged results. Streaming calls deliver tiles synchronously, so already-consumed tiles cannot be rolled back if a later callback fails.

## 4. Non-Goals & Explicit Boundaries
- Run state has no daemon Job identifier or durable identity.
- Cancellation is cooperative. A callback already running may need to return before the call drains.
- Results are not retained by `GraphContext`; optional context caches are disposable derived state.

## 5. Consequences
Graph replacement makes plans stale but does not stop unrelated Runs. Callers must keep contexts alive during direct execute calls and treat `Stale`, cancellation, and callback failures as terminal for that call. Shutdown waits for owned callbacks to retire; a slow callback therefore delays resource teardown.
