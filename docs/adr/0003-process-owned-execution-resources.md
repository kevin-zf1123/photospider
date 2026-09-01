# ADR 0003: Local Execution Resources Are Explicitly Owned

- Status: Accepted, narrowed by ADR 0015
- Date: 2026-09-01 boundary revision

## Context

An embeddable kernel must run independent graphs without creating hidden
process singletons or scaling physical workers with graph count. Queue
capacity, modeled memory, optional GPU work, cancellation, and shutdown need
one visible owner.

## Decision

Each `ExecutionContext` owns one fixed local execution composition:

- a deterministic FIFO and fixed CPU worker pool;
- an optional one-worker local GPU lane with its own deterministic FIFO;
- one nonblocking shared waiting-callback admission limit across both FIFOs;
- a frozen `OperationRegistry` shared by every invocation;
- one exact-release `ResourceLedger` for the configured modeled-byte limit.

The context configuration is immutable after construction. Zero CPU workers
selects a bounded hardware-derived count; CPU execution always exists. A zero
queue or byte limit is invalid. An embedder may create multiple independent
execution contexts or explicitly share one context across graphs.

### Admission and work ownership

Each `execute` call creates one private `ExecutionRun`. The Run owns dependency
counters, deterministic ready-step ordering, staged Values, per-step backend
residency, in-flight accounting, cancellation observation, and raw diagnostics.
`ExecutionOptions::maximum_parallelism` bounds in-flight plan steps for that
Run. `maximum_queued_tasks` is one ExecutionContext-wide limit for callbacks
accepted by either backend FIFO but not yet started; it is not duplicated per
lane. A move-only waiting token is released when a worker pops the callback,
so running callbacks do not consume the waiting limit. Enqueue rejection,
allocation failure, shutdown drop, and exception unwinding roll the token back
exactly once. This shared admission and the byte ledger provide nonblocking
backpressure.

Before invoking an operation, the Run acquires the step's complete planned
byte charge. The move-only lease releases exactly once after the attempt,
including fallback, exception, cancellation, and stale-completion paths.

### Local transfer and fallback

The CPU backend is required. When enabled, the GPU lane is another local
in-process callback lane. If a dependency Value was produced on a different
backend, execution makes an explicit immutable byte copy and records transfer
count and bytes. Residency is Run-local derived state; it is neither persistent
nor a global manager.

Unavailable GPU capability and recoverable GPU failure may fall back to CPU
only when copied operation traits permit it. The reason is an ordinary
diagnostic.

### Cancellation, exceptions, and shutdown

Every callback is fenced. Cooperative cancellation and graph-revision
currentness are checked before admission, during completion, and before final
result assembly. Late work may finish cleanup but cannot publish a caller
result after cancellation or staleness.

Destruction stops queue admission, rejects queued callbacks, releases every
dropped waiting token, wakes workers, and joins owned threads. Callers must not
race `execute` with context destruction.

## Boundary

All resources are process-local. This ADR creates no daemon Session/Job,
external scheduler, remote worker, process supervisor, plugin sandbox,
durable state, or security domain.

## Consequences

- Physical resource count follows explicit context configuration, not graph
  count.
- Queue and byte backpressure are bounded and testable.
- Transfer, fallback, stale rejection, and cleanup have one Run-local path.
- Multiple contexts can execute concurrently without a kernel-global registry.
