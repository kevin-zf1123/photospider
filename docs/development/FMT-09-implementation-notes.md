# FMT-09 revision-2 implementation notes

The original English operator specifications remain the behavioral contract.
These notes explain implementation choices; they do not relax numerical or
resource limits.

The R2 public C++ invocation layout requires package 0.25 and a complete rebuild
of C++ consumers. The installed gate rejects 0.24 package requests; C operation
ABI9 and planar C extension v1 are unchanged. Tests using the private factory or
SIMD entry link the noninstalled static test kernel, including shared builds.

## Borrowed host services and ownership

`PlanarOperationInvocation::consume_work` is available even without a managed
root. A zero charge polls cancellation/currentness. A positive charge polls,
then debits the root when configured. The host retains a sticky failure and
checks it before publication; a plugin cannot ignore the returned failure and
publish successfully. Cancellation has priority. The callback is borrowed,
serial-only and must not be retained or called concurrently. `report_numeric`
has the same lifetime/synchronization restriction. The actual transfer
refinement routes every existing work checkpoint through this explicit service.

The planar worker installs `ResourceAllocationScope` for callback-owned
`ResourceVector` temporaries, separately from the already-accounted fixed
workspace allocator. Metadata admission failure is checked **before** the
write-window commit even when a callback catches `bad_alloc`. The worker also
checks it on return; resource errors are not converted to numerical retries.

A Gray tensor without `channel_axis` has one implicit component, index 0.
Its color group uses `indices={0}`; there is no implicit second component or
alpha axis. This explains the pre-existing codec validation and its tests.

## Strict enclosure tier

The full `DirectedInterval` alias still selects the 192-word, 128-slot arena.
FMT-09 additionally instantiates the same signed dyadic machinery with 16 words.
The compact evaluator attempts 128 and 256 fractional bits and accepts only
when both directed endpoints round to the same destination bit pattern.
It does not accept a midpoint estimate, a float64 intermediate, or a tolerance.

`DirectedCapacityRetry` is internal and only emitted by the compact tier when
its arithmetic capacity or proof is insufficient. The retained full evaluator
then starts with its own 128..4096-bit policy and exact algebraic fallback.
Final square roots and nonfinite algebraic inputs go directly to the full
evaluator. Rational branches with input magnitude below 2^-128 also bypass the
compact attempt: tiny transfer toes otherwise lose their input at the initial
working precision and pay exception unwinding before repeating the wide path.
This selects an existing exact evaluator, not a different rounding rule.

Caller `Status` errors, `ExactWorkFailure`, failed admission, cancellation and
Stale never take this retry route. Both workspaces are lazy, admitted and owned
by one invocation; the worst-case bound includes both, since a mixed request
can retain a compact arena and a full arena. There is no value/result cache.
The existing constant cache remains local to the program and precision.

Three compact-only reductions keep the proof unchanged:

1. Import the certified floor of Q4096 constants by taking its prefix, with
   `[floor(c*2^p), floor(c*2^p)+1] / 2^p`. The discarded suffix does not require
   a 4096-bit temporary at a 128/256-bit working precision.
2. For nonnegative numerator and strictly positive denominator intervals,
   division is monotone: use `floor(a.low/b.high)` and `ceil(a.high/b.low)`.
   Signed or zero-crossing intervals retain the general corner algorithm.
3. Equal endpoints have identical exact logarithms, so one directed point-log
   enclosure supplies both result bounds instead of evaluating it twice.

Fixed-width integer import routines requiring the full binary64 product domain
still assert at least 68 words when instantiated. Compact multiplication checks
its raw-product capacity before the fixed-size kernel. The full alias retains
its existing capacity-error behavior for other NUM/CRV users.

The fixed golden and sweep tests are finite numerical evidence, not an
exhaustive binary32/binary64 proof. The original 4096-bit cap may still produce
an explicitly bounded failure on hard cases; it has not been removed.

## Exact gamma=2 and copy traversal

A signed gamma=2 decode is one target-format multiplication; encode is one
target-format square root with sign restored. The separate ISA object provides
AVX2 or AArch64 NEON kernels. Dispatch uses the existing capability check and
nearest/gradual-underflow environment. Integer masks quiet signaling NaNs while
preserving sign/payload; inactive NaN lanes do not become FP operands. Loads are
unaligned and only full authorized vectors are accessed; scalar tails consume
exactly the remaining entries. Semantic input must be finite. Decode also
rejects nonfinite rounded output, since valid signed power-gamma input is not
restricted to [0,1]. Raw overflow remains infinity as before.

The span specialization removes DAG arrays, gathering, workspace allocation
and per-valid-sample owning Status construction. The SIMD build flag OFF keeps
this scalar specialization so comparisons can distinguish dispatch/validation
work from the vector arithmetic itself.

Unselected planar copy spans use at most 1024 entries between host checkpoints;
selected general DAG spans remain at most 64. Unusual raw selections along the
physical width axis retain per-entry selection, without reading peer values.
No change was made to the global scheduler, task queue, cache policy or worker
parallelism.

The private factory declaration in `transfer_runtime.hpp` lets deterministic
integration tests wrap the real callback's service while executing through the
normal Workflow/compiler/executor. No test hook or clock-dependent sleep was
added to the product path.

## Reproducibility

`examples/transfer_performance/README.md` documents all three feature switches,
the independent 6,656-case sweep, every-iteration validation and ABBA driver.
Sweep input generation uses 180/360-digit powers with direct binary64 rounding;
macOS and FreeBSD therefore verify identical inputs without relying on libm pow.
The separate ISA object participates in ASan/TSan instrumentation; linking a
sanitized executable alone does not instrument an unsanitized SIMD object.
