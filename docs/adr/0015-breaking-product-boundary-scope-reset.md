# ADR 0015: Keep Compilation and Execution in the Kernel

- Status: Accepted

## 1. Core Summary (TL;DR)

Photospider separates embedded graph compilation and execution from local process orchestration. The kernel owns typed workflow data, planning, execution, resources, cancellation, and results; the daemon owns local sessions, jobs, and IPC. This boundary keeps the kernel embeddable and lets the daemon consume only the installed public package.

## 2. Mental Model & Intuition

```text
Application -> WorkflowDocument -> Compiler -> ExecutionPlan
                                         |             |
                                         v             v
                                  kernel registry   ExecutionContext
                                                       | CPU / optional GPU
                                                       v
                                                  named results

Client -> local IPC -> daemon Session / Job -> installed kernel API
                              |                    (no internal IR on wire)
                              +-> job lifecycle and temporary result lifetime
```

The compiler turns a caller-owned document into typed semantic and optimized stages, then a physical plan. An `ExecutionContext` runs that plan using bounded local workers and resources. The daemon submits work through the installed kernel facade and owns the lifetime of its ephemeral job records and results.

## 3. Formal Contracts & APIs

```cpp
class Compiler final {
 public:
  explicit Compiler(std::shared_ptr<OperationRegistry> operations);
  Result<SemanticGraphIR> analyze(const GraphSnapshot& snapshot,
                                  ResourceBindings resources = {}) const;
  Result<OptimizedGraphIR> optimize(const SemanticGraphIR& semantic) const;
  Result<ExecutionPlan> plan(const OptimizedGraphIR& optimized,
                             const PlanningOptions& options = {}) const;
};

class PHOTOSPIDER_API ExecutionContext final {
 public:
  Result<ExecutionResult> execute(
      const ExecutionPlan& plan, ExecutionBindings bindings = {},
      const CancellationToken& cancellation = CancellationToken(),
      const ExecutionOptions& options = {});
};
```

These declarations reproduce the public compiler and execution method signatures; headers and unrelated declarations are omitted. `Compiler::analyze` accepts the graph snapshot and optional resource bindings. `ExecutionPlan` is a public class.

The kernel owns `WorkflowDocument` interpretation, graph validation, semantic and optimized IR, operation traits, planning, `Value` and `Region` contracts, local execution, resource accounting, cancellation, backend selection, fallback, and result publication. `GraphContext` and `ExecutionContext` are independent objects; neither is a daemon session or a global registry entry. Stage digests and plan/cache keys are non-security identities for reproducibility and disposable derived data.

The daemon owns local IPC and ephemeral process orchestration around the installed kernel package. The kernel owns compiler IR, execution resources, values, and execution results under its public API. This ADR assigns these component boundaries; implementation details for daemon sessions, jobs, cancellation, and cleanup belong to the daemon repository.

Operations and data providers are loaded into the host process through their public C interfaces. The configured operation registry is frozen for execution. ABI validation, bounds checks, exception fencing, and cleanup protect correctness; they do not isolate untrusted code.

The kernel retains defensive correctness checks for ABI versions and structure sizes, alignment, pointer/count and array bounds, integer/allocation overflow, graph and plan validity, stale handles/completions, cancellation publication, exception fencing, and exact resource cleanup. Optional GPU failures can use CPU fallback only where the operation contract permits it. These checks do not provide process isolation.

Maintained benchmarks report raw compile/plan/execute/operation timings, selected backend, transfer counts and bytes, peak live bytes, fallback/error reason, plan/result identities, and correctness observations. When an oracle is supplied, each sample records its bounded canonical `oracle_name`; a run without an oracle is marked `unchecked`. Benchmark output does not create evidence authority, durable artifact identities, or release verdicts.

## 4. Non-Goals & Explicit Boundaries

- The kernel is a single-machine embeddable library. It does not define daemon job identity, a global queue, service status, automatic retry, or persistence.
- The daemon is a local orchestration process. A session is a process-local logical namespace, not a tenant or isolation boundary.
- Neither component provides network service, authentication, authorization, multi-tenant quotas, remote execution, or distributed devices.
- Durable jobs, checkpoints, recovery journals, durable results, artifact authority, receipts, release evidence, and operational SLO verdicts are outside the current product. The daemon's ephemeral jobs remain part of local orchestration.
- Operation and provider DSOs run with host-process authority. The system does not provide sandboxing, process isolation, cryptographic admission, or a policy-plugin product.
- Internal semantic IR, optimized IR, and physical plans are not stable wire formats.

## 5. Consequences

Kernel callers manage their own graph and execution contexts, input allocations, concurrent calls, and result lifetimes. Controlled execution buffers are bounded by context configuration; caller-owned inputs and uninstrumented process overhead are not an RSS limit. Ordinary execution observes cooperative cancellation and rejects publication from a stale graph revision. Captured frozen work follows its own captured-plan lifetime contract.

Daemon callers integrate through the installed kernel's public package and API. This kernel repository does not establish compatibility with a particular daemon revision or specify daemon cleanup and restart behavior.

Consumers must build against the installed package and match its public package and ABI versions. Kernel or ABI changes require consumer rebuilds when the public contract changes. Plugin validation can reject incompatible or malformed tables, but cannot make an accepted in-process DSO safe to execute.
