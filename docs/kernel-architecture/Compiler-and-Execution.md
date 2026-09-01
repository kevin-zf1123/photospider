# Compiler and Local Execution

## Compile stages

`Compiler::analyze` checks a current `GraphSnapshot`, bounded document counts
and text, unique node/output ids, references, ports, operation availability,
input counts, deterministic acyclic topology, and static output descriptor
inference. It publishes immutable `SemanticGraphIR` in node-id-tiebroken
topological order plus `SemanticGraphDigest`.

`Compiler::optimize` is an explicit conservative no-op in this baseline. It
copies the semantic nodes into a distinct `OptimizedGraphIR` and produces a
domain-separated `OptimizedGraphDigest`.

`Compiler::plan` copies dependency-ordered steps, selects CPU or a declared
optional local GPU backend, records estimated bytes, and produces
`ExecutionPlan`, `ExecutionPlanDigest`, and `PlanCacheKey`. No stage contains a
callback pointer, DSO handle, allocation, native device, or daemon object.
Each stage also carries a private runtime-only weak identity for the exact
frozen operation registry; it is excluded from digests and serialization.

## Execution

`ExecutionContext` owns a fixed CPU pool, an optional one-worker GPU callback
lane, bounded queues, a frozen operation registry, and a modeled-byte ledger.
`execute` creates one private `ExecutionRun` with deterministic ready-step
ordering and a caller-selected maximum parallelism.

When a dependency and consumer have different backend labels, the Run creates
a distinct validated Value by copying immutable bytes. The copy is explicit in
transfer count/bytes. Backend labels are Run-local derived state; the kernel
does not expose a native GPU handle or persistent residency registry.

Every operation result is checked against the planned element type and shape.
The execution context must use the same frozen registry that produced the
plan. Cancellation and plan currentness are checked before work, during
completion, and before result assembly. A late cancelled/stale result releases
resources without entering the caller-visible `ExecutionResult`.

## Diagnostics

Raw diagnostics include compile-stage duration, execute duration, operation
attempt timing/outcome, selected backend, transfer count/bytes, peak modeled
bytes, fallback reason, plan digest, and result digest. They are observations,
not verdicts or release evidence.
