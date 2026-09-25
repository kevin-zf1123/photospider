# Packed numeric adapter and binary64 exp restoration, 2026-09-25

## Implementation

This update follows the certified trig implementation and its FreeBSD analysis.
The baseline for these measurements is the pre-update working implementation,
including all six polynomial trig kernels, not the older SLEEF trig baseline at
`6786cb93`. Original benchmark executables were retained before rebuilding.

The point-math callback now caches its output base pointer once. A packed input
block is copied into its accounted workspace in one operation; Float32 lanes are
classified once into a 64-bit rejection mask. Rejected input bits are retained
for exact fallback and replaced with zero before SIMD arithmetic. The block
result is copied out once and only rejected lanes are repaired. No output is
published if admission, cancellation or fallback fails. The mathematical SIMD
kernels and their coefficients/FMA graphs are unchanged.

Coordinate increments are skipped only when every input port is packed. Mixed
packed/transposed binary ports and arbitrary strides keep coordinate addressing.
The generic SLEEF batch and scalar paths also use the cached output pointer and
conditional coordinate advancement. Per-block indexing and fixed arithmetic
charges, per-fallback work, cancellation boundaries and fenv restoration remain
in place. Bit copies support unaligned storage and only cover valid tail lanes.

FP64 NUM-04 exp restores SLEEF 3.9.0 `xexp` in [-80,80]. Both scalar and batch
paths retain binary64 inputs/output; the candidate is enclosed by four adjacent
binary64 steps and admitted only by the existing final FP32-scaled accuracy
guard. This is the existing accelerated Float64 contract, not a new global
four-ULP64 guarantee. Float32 NUM-04 exp still uses IQK. Exact special cases and
out-of-domain/range uncertainty retain strict evaluation.

NUM-01 also restores the binary64 SLEEF exp endpoint enclosures for its RN64
AST. This does not substitute the Float32 IQK certificate into expressions.
Final-result enclosure and strict replay preserve the original expression
success/failure semantics, including cancellation and zero-denominator cases.

The exp operation's common workspace/trait now takes the maximum of the two
batch workspaces: 2,624 bytes instead of 768 bytes in the previous Float32-only
adapter. This adds **1,856 accounted payload bytes** for either exp dtype.
The large exact workspace is still initialized, so this update does not claim
to remove the fixed small-array cost. No public ABI or profile names changed.

## Measurement conditions

FreeBSD: i9-12900, explicit Clang 22.1.7, same isolated LLVM 22 C++ runtime,
RelWithDebInfo, CPU 2, one worker, one warmup and seven timed iterations, result
cache disabled. The user configured loader `machdep.hwpstate_pkg_ctrl="0"` and
rebooted. The authoritative `percore.csv` campaign verifies per-core mode;
CPU 2 reported 4.737 GHz under sustained load. No package switching is used in
that campaign. The persistent configuration remains the user's per-core setting.
See the [preceding HWP diagnosis](freebsd-wsl-diagnosis.md) for why older
2.922 GHz results are not the baseline here.

macOS: Apple M5, Homebrew Clang 21.1.3, same SDK/build as the pre-update binary.
No CPU affinity or fixed clock is imposed on macOS. Each platform has three
paired large-array process rounds, alternating before/after order. Below are
medians of the three process medians, not confidence intervals. Compile/freeze
and full output checks are outside timing; public includes execute/collect,
core includes the direct callback's allocation, gather and store.

Primary trig/Float32 exp timings omit the optional managed ledger. The public
Float64 unary fixture enables it. Those distinct accounting paths are not
subtracted from each other. Timing and hardware profiling run separately;
there are no concurrent builds or benchmark campaigns on the measured host.

## Large ordinary inputs

N=1,048,576; public milliseconds. Sin/cos/sinc/sincpi use [-1,1], sinpi/cospi
[-1/4,1/4], and Float32 exp [-10,10].

| Function | FreeBSD before → after ms | Speedup | M5 before → after ms | Speedup |
| --- | --- | --- | --- | --- |
| sin | 4.995 → 2.425 | 2.06x | 3.619 → 1.491 | 2.43x |
| cos | 4.994 → 2.412 | 2.07x | 3.547 → 1.442 | 2.46x |
| sinpi | 4.985 → 2.413 | 2.07x | 3.606 → 1.464 | 2.46x |
| cospi | 4.975 → 2.422 | 2.05x | 3.483 → 1.471 | 2.37x |
| sinc | 4.952 → 2.385 | 2.08x | 3.517 → 1.326 | 2.65x |
| sincpi | 5.990 → 3.777 | 1.59x | 5.170 → 3.497 | 1.48x |
| exp | 4.195 → 1.971 | 2.13x | 3.655 → 1.526 | 2.39x |

FreeBSD core medians also improve: sin 4.715 → 2.144 ms, sincpi
5.731 → 3.518 ms, and Float32 exp 3.908 → 1.696 ms. The smaller sincpi gain is
consistent with its larger unchanged arithmetic-kernel share.

Float64 exp in the public managed unary fixture, x=1 and N=256, changes from
**9.939 ms to 52 us (191.1x)**. N=1 changes from 72 to 34 us. A preceding run
measured 9.944 ms to 52 us at N=256. These are selected ordinary-input fixtures;
outside [-80,80] the strict exp engine remains the relevant cost. The new FP64
oracle checks broader input behavior independently of these timing inputs.

## Small arrays, layouts and fallback

Short seven-sample small-array runs had inconsistent startup/frequency noise,
including apparent regressions around 64 elements. Follow-up uses three
alternating-order pairs, 1,001 iterations for N<=65 and 101 for N=4096.
FreeBSD sin medians, microseconds:

| N | Public before → after us | Core before → after us |
| --- | --- | --- |
| 1 | 30.224 → 29.916 | 6.953 → 6.988 |
| 64 | 30.762 → 30.122 | 7.245 → 7.063 |
| 65 | 30.682 → 30.186 | 7.275 → 7.095 |
| 4096 | 49.778 → 39.411 | 25.024 → 14.928 |

Small fixed overhead is essentially unchanged; the full arithmetic workspace
allocation/initialization remains. No sub-percent small-array claim is intended.

At N=65,536, FreeBSD sin core with unaligned contiguous storage improves
297.642 → 138.946 us, negative stride 1300.230 → 1086.480 us, and zero stride
1178.170 → 1062.910 us. Sinc shows the same pattern. Zero-stride data repeats
one logical value; this differs from the random dense distribution. Nonstandard
layouts are measured through the direct callback, not unsupported workflow
input declarations.

With every sixteenth input replaced by 2 radians, N=4,096 sin core is
37.055 → 36.209 ms and sinc core 40.282 → 39.185 ms. These single-process domain
rows show only modest improvement; strict wide-integer refinement still
dominates. The adapter change does not expand the admitted mathematical domains.

## Hardware evidence

The initial profiling attempt failed because reboot had unloaded `hwpmc`.
After the user entered sudo in a separate Terminal.app window, the module was
loaded and process counters/sampling reran successfully as the ordinary user.
Only the successful `percore-*` recordings are used below.

For sin core, N=1,048,576 and 500 profile repetitions, whole-process counts are:

| Counter | Before | After | Reduction |
| --- | --- | --- | --- |
| Retired instructions | 60,604,141,609 | 21,810,999,233 | 64.0% |
| Unhalted cycles | 11,029,987,237 | 5,004,256,721 | 54.6% |

Counter runs include setup and the single warmup output check; they are not
isolated polynomial counters. Their core medians were 4.655 → 2.097 ms.
The instruction reduction corroborates removal of adapter work independently
of wall-clock noise.

Separate public cycle samples, 1,000 repetitions, use interval 1,000,000 cycles.
There are 20,663 displayed samples before and 8,831 after. Unknown-function
samples are 55/20,663 and 91/8,833 parsed samples; two post-update samples have
dubious frames. Selected leaf shares:

| Leaf | Before | After |
| --- | --- | --- |
| Point-math callback | 73.08% | 58.46% |
| MutableBuffer::data | 9.54% | Below prominent sampled leaves |
| memcpy | 6.45% | 16.88% |
| trig SIMD | 4.57% | 10.70% |
| memset | 3.44% | 7.36% |

Fractions are relative to a much shorter execution. The kernel has 944 versus
945 samples with the same repetition count, while callback samples fall from
15,100 to 5,163. Memcpy remains a target, but its higher fraction alone is not
an absolute-time regression. Inlining and sampling skid limit line-level cost
claims. Small-array lazy scratch allocation was not attempted in this update.

## Correctness and review

Both native Apple/NEON and FreeBSD/AVX2 passed:

- Six trig MPFR corpora: 4,742 cases each for sin/cos/sinpi/cospi/sinc and
  10,333 for sincpi, each in six partitions. Observed maximum ordered steps
  remain 1/1/2/1/1/0, respectively.
- Float32 exp: 20,503 MPFR cases in six partitions, maximum two ordered steps.
- The above drivers' layout, fenv, work/capacity, cancellation and owner-release
  checks; mixed fallback admission remains charged once.
- Expanded `math_batch`: exp plus the six existing SLEEF operations in both
  dtypes, ±80 neighbors, special values, reverse/zero/unaligned layouts,
  2D transposed/mixed ports, partitions and exact work/capacity thresholds.
- New `exp64_oracle.py`: 1,096 independent MPFR public cases, including random
  binary64 inputs, ±80 adjacent inputs, strict fallback/special bits and
  non-Float32-representable inputs. Sampled maximum binary64 distance was one
  step on both hosts; this empirical result does not replace the certificate.
- Public unary: 7,524 independent exact/MPFR cases; expression: 715 independent
  coordinate/stepwise Fraction/MPFR cases, including sensitive compositions.
- Focused `test_numeric_operations` and `test_expression_operations`, 2/2 on
  each host. Changed C++ passed ClangFormat 21/cpplint; the new Python oracle
  compiles and the diff whitespace check passes.

Independent read-only review examined pointer lifetime/bounds, packed and mixed
layouts, mask/tails, sNaN handling, admission/cancellation, workspace traits,
SLEEF error enclosure and restored expression certification. No blocker or
required finding remained. No full release/sanitizer or new WSL matrix is claimed.

## Artifacts and reproduction

Local results: ignored `build/num04-exp/adapter/`; FreeBSD readable records are
under its `freebsd/` subdirectory. Remote root:
`/home/alex/photospider-trig-20260925/adapter/`.

The retained `trig-before`, `exp-before` and remote `unary-before` binaries
capture the pre-update implementation. `measure.py CURRENT_BIN_DIR OLD_BIN_DIR
OUTPUT.csv` reproduces the paired timing campaign with platform-appropriate
FreeBSD CPU-2 affinity. Use `percore.csv` for the final FreeBSD table and
`mac.csv` for Apple. `small.py` records the longer small-array confirmation.
Only normal dense rows from `mac.csv` are comparable: the retained Apple
baseline driver predates the optional distribution/layout arguments and ignores
them. Its 16 extra mixed/non-dense rows are excluded. All 116 comparable Apple
rows, 132 final FreeBSD rows and 48 longer small-array rows have identical
before/after checksums. The FreeBSD before/after drivers support the same flags.
Successful hardware evidence is in `percore-count-*`, `percore-*.callgraph`,
`percore-*.analysis` and raw remote `.pmc` files. Earlier failed profiler logs
and the initial `freebsd.csv` are retained but not used as final evidence.

Numerical acceptance is reproducible with the maintained exp/trig corpus
drivers, `math_batch`, `unary_oracle.py`, `expression_oracle.py` and
`exp64_oracle.py`. The analysis makes no changes to the user's loader settings.
