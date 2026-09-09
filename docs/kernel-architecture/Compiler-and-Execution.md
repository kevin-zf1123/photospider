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
the plan uses the declared `estimated_bytes` for a non-densely-representable
output, with at least one element of capacity. Actual allocations must fit
the declared bound; no dense allocation is inferred for that case. Explicit `Region::element_count()` remains
checked and may still return overflow for the same multi-dimensional logical
shape. A stride-free C DSO Fixed descriptor is separately required to be
densely representable at load: its checked uint64 contiguous products must
produce total bytes `B > 0`, with `B - 1 <= INT64_MAX` and `B <= SIZE_MAX`.
This DSO-only address/allocation boundary does not constrain a C++ broadcast
descriptor.

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
shared controlled-buffer budget. Both FIFOs share the single nonblocking
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

Run-loop observation of cancellation or staleness is itself a no-throw
boundary. If the Run first observes either stop while callbacks remain active
and constructing the owned diagnostic or `Status` fails, it records the same
prioritized code with an empty message while retaining the Run mutex. This
fallback neither reacquires that mutex nor retires an in-flight slot: it stops
new admission and waits until every callback has retired normally. The
maintained regression proves that ownership directly: while the sole CPU
worker is occupied, it admits the target CPU callback and then a separate CPU
Run as its FIFO successor. Successor completion proves the target callback has
finished abandonment; the target future must still remain incomplete while an
independent GPU callback is gate-held.

Every callback popped from either backend FIFO enters the Run mutex and uses
that same no-throw external-stop observation before copying dependencies,
transferring Values, allocating reserved buffers, or invoking the operation. An
existing or newly selected failure makes that worker retire exactly its own
in-flight slot through the abandonment path. Direct CPU work, GPU work, and a
GPU-to-CPU fallback all converge on this queued-attempt admission cutoff; the
observation itself never retires a slot and `GraphContext` does not register or
notify Runs. Cancellation or replacement after the cutoff may still race with
transfer, resource acquisition, or entry into a non-preemptible in-process
operation. That later interval remains cooperative/best-effort rather than a
global mutex or forced-preemption guarantee, while completion and final result
publication continue to reject every observed cancelled or stale outcome.

At the operation-registry boundary, invocation validation preserves the order
of operation lookup, input/demand counts, per-input validity and demand bounds,
parameters, cancellation, closed CPU/GPU backend vocabulary, backend
capability, and static descriptor compatibility. No input descriptor is read
before its `Value` is valid. Unknown backend representations are
`InvalidArgument` and enter neither C++ nor DSO code; known unsupported
backends remain `BackendUnavailable`. After capability succeeds, the registry
precomputes one expected Scalar/Fixed/Preserve/Match output descriptor.
Preserve first-input type conflicts and Match type/shape conflicts return
`TypeMismatch` before callback entry. The same descriptor is reused after the
callback to validate output type, shape, and requested Region, including a
default-invalid output. This does not duplicate the Run's plan-derived demand
coverage checks or the DSO adapter's contiguous-layout/facet views.

When a dependency and consumer have different backend labels, the Run creates
a distinct validated Value by copying immutable bytes. The copy is explicit in
transfer count/bytes. Backend labels are Run-local derived state; the kernel
does not expose a native GPU handle or persistent residency registry.

Every operation result is checked against the planned element type and shape.
Each producer Value must cover the consumer's planned input demand before
transfer or callback entry; callbacks and ABI v5 input views receive that exact
demand. Image and regional-source Runs lazily materialize only demanded tiles;
Whole/effect boundaries materialize once per Run. See [Region semantics](Region-Semantics.md).
The execution context must use the same frozen registry that produced the
plan. Cancellation and plan currentness are checked before work, during
completion, before result assembly, and once more after all named Values,
diagnostics, plan/result digests, and execute timing have been assembled. The
Run holds its mutex for this final cancellation-then-currentness recheck;
passing it immediately before the sole success return is the success-
publication linearization point. A late cancelled/stale local result and its
diagnostics are discarded, and all Values and resource owners retire without
entering the caller-visible `ExecutionResult`.

An operation ABI v5 callback can distinguish ordinary failure from backend
unavailability without changing its C signature or descriptor layout. The
executor retries on CPU only when an optional GPU attempt returns the explicit
backend-unavailable result without invoking its output sink and copied traits
allow fallback. An output-publication attempt makes backend unavailability
terminal: accepted output is a contract failure and rejected output retains
the sink failure. Ordinary failure and unknown nonzero callback results fail
the Run without a CPU attempt.

## Diagnostics

Raw diagnostics include compile-stage duration, execute duration, operation
attempt timing/outcome, selected backend, transfer count/bytes, peak allocated
bytes, fallback reason, plan digest, and result digest. They are observations,
not verdicts or release evidence.

## Runtime input lowering and execution

Schema 2 validates every input declaration before semantic publication and
copies its canonical table through semantic IR, optimized IR and plan. Ordered
sources retain node/declaration tags. Scalar ports require direct Float32 {1}
workflow inputs, exact empty facets and finite inclusive intervals; all
consuming intervals must have nonempty intersection during analyze. Image
consumers require the exact profile from a declaration or a producer's image
output guarantee. Generic producers do not implicitly acquire that guarantee.

`execute(plan, bindings, cancellation, options)` snapshots input names and Value
metadata, checks the name multiset, then Values in declaration-id order, then
all direct scalar constraints before the first callback or transfer. Image and
mask pixels are validated only in consumed regions, before their consuming callback.
Entry rejects default/stale/foreign-registry plans as Stale before observing
bindings or the token. After entry, cancellation precedes Stale and ordinary
binding failure. Long numeric scans periodically check cancellation and graph
currentness. Run-owned snapshots survive all admitted callbacks; returned
Values own their immutable bytes independently. Runs share no mutable bindings
or results and do not reuse output by plan digest.

External inputs begin with a CPU backend label and use the existing explicit
transfer/fallback path. Caller-preexisting input bytes are outside maximum_live_bytes; source-read buffers
and collected or streamed output buffers are inside it.
Each step bounds output capacity plus workspace_bytes and
workspace_input_multiplier (0..16) times demanded input bytes. Images no longer
reserve a duplicate sink copy. The executor reserves a conservative complete
working set before callbacks, including possible transfers, then each output,
copy and scratch allocation obtains a sublease. Per-invocation limits prevent
scratch from consuming another step's reserved capacity. Temporary competition
waits only for active work, observes cancellation/currentness, and fails when
externally retained results leave insufficient capacity.

Fan-out/repeated edges count every reader. Callback-local inputs retire before
completion; producer slots clear after their last reader completes unless they
are named outputs. Shared storage has one lease. Returned Values retain their
allocation after the Run or ExecutionContext ends. Completion returns unused
reservation capacity; final owner destruction releases retained capacity.
Diagnostics distinguish planned_peak_bytes, actual peak_live_bytes,
retained_input_bytes and peak_active_tasks. Metadata/stacks/process RSS are not
part of the payload budget. `test_memory_liveness` exercises gated fan-out,
post-context results, budget recovery and exact/one-byte-short workspace limits.
RawBenchmarkOptions.bindings is copied once on entry and supplied to each
independently compiled sample.

Direct `OperationRegistry::invoke` uses the same input-demand derivation as
physical planning: it rejects partial-channel RGBA outputs, incomplete Whole
outputs, insufficient halo, mismatched mask spatial shape and Value coverage
shorter than the claimed demand before callback allocation. Whole caches retire
after their final remaining boundary/output reader, including chains of Whole
operations; `test_regional_execution` checks a three-node chain at 16/15 bytes.

Run completion waits for queue callback ownership to retire as well as the
logical in-flight steps. A final scope clears all Run-held Value/binding owners
on success, cancellation and failure. Releasing the caller's returned result
therefore immediately returns its payload capacity even if queue metadata is
still retiring. The private callback-body gate in `test_memory_liveness` checks
this boundary for successful and cancelled calls with an eight-byte budget.
