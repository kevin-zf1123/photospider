# Compiler and Local Execution

## Compile stages

`Compiler::analyze` checks a current `GraphSnapshot`, bounded document counts
and text, unique node/output ids, references, ports, operation availability,
input counts, each operation's closed required typed parameter schema,
deterministic acyclic topology, and static scalar/preserve/match/fixed output
descriptor inference. Unknown, missing, or wrong-type parameters fail before
IR publication; built-ins do not synthesize defaults. Analysis publishes
immutable `SemanticGraphIR` in node-id-tiebroken
topological order plus `SemanticGraphDigest`.

Fixed inference validates a logical nonzero rank-1..8 descriptor and does not
require its dense element or byte product to fit. A C++ embedding operation may
materialize that descriptor as a valid strided or zero-stride broadcast Value;
the plan records the independently declared `estimated_bytes`, so modeled
admission and peak diagnostics reflect actual callback materialization rather
than an implicit dense allocation. Explicit `Region::element_count()` remains
checked and may still return overflow for the same multi-dimensional logical
shape. A stride-free C DSO Fixed descriptor is separately required to be
densely representable at load.

Each schema-valid Float64 parameter enters canonical stage identity using the
exact copied IEEE-754 binary64 bits in fixed little-endian order. Signed zero
is preserved, so sign-sensitive callbacks cannot share semantic, optimized,
plan, or cache-key identity. The compiler introduces no finite-only rule and
does not normalize NaN payloads or infinities.

`Compiler::optimize` is an explicit conservative no-op in this baseline. It
copies the semantic nodes into a distinct `OptimizedGraphIR` and produces a
domain-separated `OptimizedGraphDigest`.

`Compiler::plan` copies dependency-ordered steps, selects CPU or a declared
optional local GPU backend, records estimated bytes, and propagates optional
named output Regions backward into per-step output/input demands using Whole,
elementwise-exact, or clipped Halo rules. It produces
`ExecutionPlan`, `ExecutionPlanDigest`, and `PlanCacheKey`. No stage contains a
callback pointer, DSO handle, allocation, native device, or daemon object.
Each stage also carries a private runtime-only weak identity for the exact
frozen operation registry; it is excluded from digests and serialization.

## Execution

`ExecutionContext` owns a fixed CPU pool, an optional one-worker GPU callback
lane, one deterministic FIFO per lane, a frozen operation registry, and a
modeled-byte ledger. Both FIFOs share the single nonblocking
`maximum_queued_tasks` admission for callbacks that have not started. A worker
releases the move-only admission token before entering the callback, so running
callbacks do not occupy the waiting bound; rejection, exception, and shutdown
drop paths release it exactly once. `execute` creates one private
`ExecutionRun` with deterministic ready-step ordering and a caller-selected
maximum parallelism.

The Run has one first-failure linearization under its mutex. Every scheduler,
waiting-admission, backend-queue, and callback failure rechecks the cooperative
token and plan currentness immediately before first storage: cancellation
outranks graph `Stale`, which outranks the original failure. The selector uses
only scalar and currentness observations, allocates nothing, and is reused by
the no-throw diagnostic-construction fallback. In the absence of either stop,
an unavailable GPU whose copied traits explicitly deny fallback remains
`BackendUnavailable`; ordinary admission and queue rejection retain their
original category.

When a dependency and consumer have different backend labels, the Run creates
a distinct validated Value by copying immutable bytes. The copy is explicit in
transfer count/bytes. Backend labels are Run-local derived state; the kernel
does not expose a native GPU handle or persistent residency registry.

Every operation result is checked against the planned element type and shape.
Each producer Value must cover the consumer's planned input demand before
transfer or callback entry; callbacks and ABI v2 input views receive that exact
demand. The executor still materializes complete Values.
The execution context must use the same frozen registry that produced the
plan. Cancellation and plan currentness are checked before work, during
completion, and before result assembly. A late cancelled/stale result releases
resources without entering the caller-visible `ExecutionResult`.

An operation ABI v2 callback can distinguish ordinary failure from backend
unavailability without changing its C signature or descriptor layout. The
executor retries on CPU only when an optional GPU attempt returns the explicit
backend-unavailable result without invoking its output sink and copied traits
allow fallback. An output-publication attempt makes backend unavailability
terminal: accepted output is a contract failure and rejected output retains
the sink failure. Ordinary failure and unknown nonzero callback results fail
the Run without a CPU attempt.

## Diagnostics

Raw diagnostics include compile-stage duration, execute duration, operation
attempt timing/outcome, selected backend, transfer count/bytes, peak modeled
bytes, fallback reason, plan digest, and result digest. They are observations,
not verdicts or release evidence.
