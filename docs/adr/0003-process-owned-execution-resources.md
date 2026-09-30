# ADR 0003: Execution Contexts Own Local Resources

- Status: Accepted

## 1. Core Summary (TL;DR)
An `ExecutionContext` owns bounded local workers, callback admission, caches, and resource accounting. Each call creates private run state and keeps its bindings and results separate from other calls. This makes resource sharing explicit without creating graph-count-scaled worker pools.

## 2. Mental Model & Intuition

```text
                  +--> CPU workers / FIFO --+
ExecutionContext |                           |--> Run callback --> Values/results
                  +--> optional GPU lane ----+
                  shared admission, allocator, caches

Run A: dependencies, cancellation, staging, diagnostics
Run B: dependencies, cancellation, staging, diagnostics
```

Runs compete for the context's configured resources. CPU execution is available; native GPU execution depends on the host and configuration. Per-run scheduling and dependency state remain private.

## 3. Formal Contracts & APIs

```cpp
struct ExecutionContextConfig {
  std::uint32_t cpu_workers = 0;
  bool gpu_enabled = false;
  std::uint32_t maximum_queued_tasks = 1024;
  std::uint64_t maximum_live_bytes = 256U * 1024U * 1024U;
};
struct ExecutionOptions { std::uint32_t maximum_parallelism = 0; };
class ExecutionContext {
 public:
  Result<ExecutionResult> execute(const ExecutionPlan&, ExecutionBindings,
      const CancellationToken&, const ExecutionOptions&);
};
```

The full configuration also controls demand work, result retention, managed capacity, optional disk cache, and scheduler observations. Construction rejects zero queue and live-byte limits and validates the other configured bounds. Zero CPU worker count resolves to a bounded hardware-derived count; zero `maximum_parallelism` uses the context CPU worker count. The caller must freeze the operation registry before construction, and the context retains it.

CPU and GPU callbacks share one waiting limit. A worker releases an ordinary callback slot when it starts that callback. A staged CPU job keeps its slot until its tile callbacks retire and the job leaves the queue.

The context charges controlled buffers until their final owner retires. Caller-preexisting immutable input bytes are outside the live-byte payload limit.

## 4. Non-Goals & Explicit Boundaries
- Resources are process-local. The context does not create daemon jobs, remote workers, or a process-wide singleton.
- The configured byte model covers instrumented controlled resources, not process RSS, OS overhead, or arbitrary plugin allocations.
- Callers must keep direct calls from racing context destruction. Cancellation is cooperative; it does not preempt a callback.

## 5. Consequences
A full waiting queue or exhausted managed capacity can reject work with a typed failure; callers should bound concurrent Runs and size limits for their workloads. A slow callback occupies a worker until it returns. Context destruction closes admission and joins owned workers, so shutdown waits for callbacks to retire. Optional caches consume configured memory and can be cleared.
