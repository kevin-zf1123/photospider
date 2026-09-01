# ADR 0007: Execution Runs and Local Resources Have Separate Owners

- Status: Accepted, narrowed by ADR 0015
- Date: 2026-09-01 boundary revision

## Context

Independent graphs may share local workers without sharing source state.
Likewise, a cancelled or stale execution must release resources without
publishing its staged result. Graph, Run, and physical-resource lifetimes must
therefore remain distinct.

## Decision

`GraphContext` owns only a copied `WorkflowDocument`, its current nonzero
revision, and snapshot-currentness state. It owns no worker, device queue,
result registry, or execution lifetime.

`ExecutionContext` owns the fixed local pools, bounded queues, frozen operation
set, and modeled-byte ledger described by ADR 0003.

Each `ExecutionContext::execute` call creates one source-private
`ExecutionRun`. The Run owns:

- the immutable plan reference and captured currentness predicate;
- dependency counts, deterministic ready-step priority, and per-call
  parallelism;
- intermediate Values and their Run-local backend labels;
- cooperative cancellation observation and the first terminal failure;
- operation timing, transfer, fallback, peak-byte, and result diagnostics.

The kernel defines no public or daemon-shaped Run identifier. Run identity is
object ownership inside one synchronous `execute` call.

### Completion and publication

An operation completion is accepted only while the caller token is not
cancelled and the plan's captured graph revision remains current. A rejected
late completion retires its callback and byte lease but cannot release a
dependent step or enter the result map.

Final result assembly occurs once, after all requested outputs are available.
The same cancellation/currentness gates run again before returning
`ExecutionResult`. Failure returns one typed status and discards staged output.
Results are caller-owned in-memory Values; the graph context does not retain or
publish them.

### Determinism and fallback

Ready steps are ordered by plan index, and dependencies are released only by a
successful predecessor. CPU is required. A GPU attempt may fall back to CPU
only when operation traits allow it; both attempts remain visible in raw
diagnostics.

## Boundary

This Run is not a daemon Job and has no queue/status/result identity outside
the call. There is no retry, attempt record, persistence, remote execution,
external scheduler, policy DSO, or security authority.

## Consequences

- Graph replacement invalidates old work without stopping unrelated contexts.
- Shared local pools do not imply shared graph or result state.
- Cancellation, exception, and staleness have exact no-publication paths.
- Daemon orchestration can wrap the public call without becoming kernel state.
