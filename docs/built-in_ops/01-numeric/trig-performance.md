# Certified SIMD trigonometric kernels, 2026-09-24

This update follows the NUM/CRV batch commit `6786cb93`. It accelerates the
existing Float32 NUM-04 profiles on macOS ARM NEON and Linux x86 AVX2/FMA.
The public definitions, four ordered-step bound, exact special/landmark rules,
and strict default remain unchanged. Float64 inputs keep their existing paths;
these Float32 certificates are not used as NUM-01 RN64 AST enclosures or as
independent per-tap allowances in CRV filters.

## Algorithms and proved domains

| Function | Polynomial/identity | Admitted Float32 arguments |
| --- | --- | --- |
| sin | `x*P(x*x)`, sharing P with sinc | zero, or `2^-120 <= abs(x) <= 1` |
| cos | even degree-12 Taylor polynomial | `abs(x) <= 1` |
| sinpi | `x*Ppi(x*x)`, pi powers in coefficients | zero, or `2^-120 <= abs(x) < 1/4` |
| cospi | even polynomial with pi powers in coefficients | `abs(x) < 1/4` |
| sinc | direct even degree-10 polynomial | `abs(x) <= 1` |
| sincpi | exact integer reduction and central polynomial | all finite Float32 |

The radian sinc polynomial is

```
P(t) = 1 - t/3! + t^2/5! - t^3/7! + t^4/9! - t^5/11!
```

Its SIMD core is one square and five explicit FMAs, with no sine call or
division. Sin adds one multiplication whose rounding is proved separately.
Cos uses six FMAs. Sinpi/cospi coefficients approximate the exact pi-scaled
function of the original binary input; a rounded `pi*x` is not substituted.
The exact quarter-turn roots continue through the algebraic implementation.

For normalized sinc, first use evenness and let `a=abs(x)`. Every finite
Float32 with `a>=2^23` is an integer and returns +0. Otherwise compute
`n=round_even(a)` and `r=a-n` exactly. Sterbenz applies when `n>=1`; the `n=0`
case simply has `r=a`. Then

```
sincpi(x) = (-1)^n * (r/a) * sincpi(r),  abs(r) <= 1/2.
```

A degree-16 even Taylor polynomial evaluates the central sincpi. Binary64
SIMD intermediates are used for this polynomial, hardware division and final
multiply, followed by RN32. This deliberately spends additional precision on
the division/product budget. It uses two NEON or four AVX2 lanes, while the
other kernels use four/eight Float32 lanes. The `n=0` branch returns the central
polynomial directly, including sincpi(±0)=1; nonzero integers explicitly return
positive zero. Odd/even integer parity supplies the sign bit. No approximate
reciprocal or rounded large `pi*x` reduction is used.

Nonfinite inputs are classified by bits and retain the existing payload,
classification and signed-zero rules. Rejected lanes in a batch are replaced by
safe zero operands before SIMD and evaluated by the existing scalar semantic
layer. Finite sincpi needs no strict refinement after admission. Other functions
retain their existing full-domain fallback beyond the listed fast intervals.

## Analytic certificate

Run `python3 examples/numeric_workflow/trig_bound.py`. It reads the actual C++
hexadecimal coefficients and uses exact Python Fractions, not sampled libm
errors. Pi is enclosed using Machin's identity and alternating atan series.
The certificate bounds coefficient quantization, the square, every Horner FMA,
the Taylor remainder, any final multiply/divide, and strict-reference rounding.
It checks final range invariants as well as the accuracy budget.

| Function | Analytic upper bound on ordered steps | Largest observed MPFR distance, both platforms |
| --- | ---: | ---: |
| sin | <2.981927 | 1 |
| cos | <1.597639 | 1 |
| sinpi | <3.417932 | 2 |
| cospi | <1.596624 | 1 |
| sinc | <1.318272 | 1 |
| sincpi, full finite Float32 | <2.000002 | 0 |

Sinc/cos outputs on the small domains stay above 0.5, making an absolute spacing
bound valid. Sine instead uses a relative error and the minimum intervening
representable spacing, including binade crossings. Its tiny-input exclusion
ensures normal outputs. For sincpi, the residual is itself representable in
Float32, so its square is exact in binary64. Outside the central interval,
nonzero `abs(r/a)>2^-24`; hence every noninteger result stays far above the
Float32 subnormal range. Integer zeros are handled exactly. The observed zero
error in the sincpi corpus is not a proof of correct rounding for every input;
the maintained guarantee is the analytic bound above and the public four-step
contract.

## Comparable performance

The baseline is a complete source archive of `6786cb93`, including the preceding
batch improvements, built with its own test kernel. Both variants use the same
`trig_benchmark.cpp`, the same private operation factory/key and the same public
WorkflowDocument execution path. The baseline defines
`PHOTOSPIDER_TRIG_BASELINE` only to disable the unavailable raw polynomial mode.
There is no mixing of old inline headers with the current kernel archive.

Hosts: Apple M5, macOS 27.2, Homebrew Clang 21.1.3, macOS 27.0 SDK in both
compared builds; Intel i9-12900, Ubuntu WSL kernel 5.15.167.4, Clang 18.1.3,
AVX2/FMA, pinned to CPU 2. Builds use RelWithDebInfo (-O2 -g), the private SIMD
object uses -O3, and fast-math remains disabled. An initial macOS baseline
selected a different SDK; it was rebuilt with the current sysroot and the
complete macOS timing sequence was rerun for the table below.

One warmup plus seven measured samples, medians, seed 404. Public runs use one
CPU worker, result cache off, a 1 GiB public live-byte ceiling, dependency work
limits 2^50 and zero dependency cache-proof work. Compile/freeze and complete
output smoke checks are outside timing. The optional managed resource ledger
is unset in timing; budget behavior is tested separately. Input is uniform in
[-1,1], except sinpi/cospi in [-1/4,1/4]. Public includes execute/collect; core
includes callback allocation/gather/store; raw includes only preallocated SIMD
arrays and the environment scope. Profiler recordings are separate from timing.

Public medians at N=1,048,576, milliseconds:

| Function | M5 baseline | M5 new | Speedup | Linux baseline | Linux new | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| sin | 38.959 | 3.717 | 10.48x | 74.241 | 4.752 | 15.62x |
| cos | 31.968 | 3.378 | 9.46x | 68.272 | 4.814 | 14.18x |
| sinpi | 180.982 | 3.549 | 50.99x | 298.693 | 4.487 | 66.57x |
| cospi | 177.638 | 3.344 | 53.12x | 295.972 | 4.613 | 64.16x |
| sinc | 141.417 | 3.460 | 40.88x | 269.076 | 4.419 | 60.89x |
| sincpi | 196.961 | 5.006 | 39.35x | 353.259 | 6.192 | 57.05x |

For wide sincpi, N=262,144 with inputs in [-1024,1024]:

- M5: 51.545 ms -> 1.037 ms (49.71x).
- Linux: 88.555 ms -> 1.548 ms (57.20x).

At N=1,048,576, current raw kernel medians were:

| Function | M5 raw ms | Linux raw ms |
| --- | ---: | ---: |
| sin | 0.182 | 0.205 |
| cos | 0.186 | 0.188 |
| sinpi | 0.182 | 0.214 |
| cospi | 0.185 | 0.201 |
| sinc | 0.191 | 0.177 |
| sincpi | 2.175 | 1.825 |

The raw/core/public CSV also covers N=4,096 and 262,144. Fixed baseline-first
process order, WSL scheduling variance and one timing sequence per host limit
small-difference claims. Speedups describe these admitted ordinary workloads;
they are not full-domain or all-CRV performance claims. Raw has no old-backend
comparison and cannot isolate the polynomial's cost relative to SLEEF alone.

The gain combines SIMD batching, narrower arithmetic for five kernels and
removal of per-element dynamic interval certification in favor of the proved
static domain. Sinc additionally avoids division, and sincpi avoids repeated
sine/cosine enclosures and division interval propagation.

## Memory and profiling

At N=1,048,576, reported public managed Payload peak is 8,599,440 bytes for
sin/cos in both variants. These operators retain the existing 2,624-byte maximum
scratch for their Float64 SLEEF branch, although the Float32 block uses only
768 bytes. Sinpi/cospi/sinc/sincpi add 768 bytes, increasing public peak from
8,596,816 to 8,597,584 bytes on both hosts. This is not RSS or allocator overhead;
new process RSS was not measured. Every block is at most 64 samples, fixed
indexing/arithmetic work is admitted before SIMD, and failures publish no output.

Separate six-second M5 Time Profiler recordings used sincpi, N=262,144,
span 1024. The SDK-matched baseline had 5,864 execution-chain samples:
34.28% included `nextafter` and 17.10% included the SLEEF adapter. The new
recording had 5,757 execution-chain samples: 26.91% included the SIMD adapter
and 26.47% had `sincpi_reduced` as the leaf; callback gather/store and buffer
access accounted for much of the remainder. No new-path samples included
SLEEF or nextafter. Inclusive categories overlap and are not additive phase
time. Both traces saved/exported successfully; Instruments terminated the
repeating targets at the configured time limit. These sample proportions
support the overhead explanation and are not speedup measurements.

WSL hardware counters were unavailable in the preceding exp investigation
(`perf_event_open` returned EPERM); this update makes no instruction/cycle or
microarchitectural attribution from Linux timings.

## Validation and reproduction

Each platform passed 4,742 independent MPFR cases for each of sin/cos/sinpi/
cospi/sinc and 10,333 sincpi cases, each repeated with partitions
1/3/7/64/65/257. The corpus includes zeros, NaN payloads, infinities, subnormals,
fast-domain edges, quarter/half/integer neighbors, exponent bins and raw words;
sincpi adds wide side lobes and neighbors around powers of two up to 2^23.
Scalar and batch outputs are bit-identical within each profile. Tests cover
unaligned/negative/zero strides, all four host rounding modes and preexisting
flags, public capacity/work failure, active cancellation and release, and exact
mixed fast/fallback work totals. The new generic batch fixture also passed.

The public unary oracle passed 7,524 cases on each platform, including Float64
and rational-pi paths; the two focused numeric/expression CTests passed. The
20,503-case exp regression also passed on macOS after the shared float batch
adapter changed. The trig corpus was generated with native MPFR 4.2.2 and used
on both hosts; the independent public unary oracle used MPFR 4.2.2 locally and
4.2.1 on WSL. No full release, sanitizer or installed-consumer matrix was run.

```sh
cmake -S . -B build
cmake --build build --target photospider_numeric_trig_benchmark \
  photospider_numeric_math_batch photospider_numeric_unary -j 8
python3 examples/numeric_workflow/trig_bound.py
python3 examples/numeric_workflow/trig_oracle.py build/trig-corpus
for f in sin cos sinpi cospi sinc sincpi; do
  build/examples/numeric_workflow/photospider_numeric_trig_benchmark \
    check "$f" "build/trig-corpus/$f.bin"
done
python3 examples/numeric_workflow/trig_measure.py \
  build/examples/numeric_workflow/photospider_numeric_trig_benchmark
```

For baseline comparison, archive `6786cb93` into an ignored source directory,
provide the same pinned `third_party/sleef` source, and add the current
`trig_benchmark.cpp` as an executable linked to that tree's
`photospider_test_kernel`, with private include paths `plugins/ops` and
`src/lib`, C++17, `-fno-fast-math -frounding-math -ffp-contract=off`, and
`PHOTOSPIDER_TRIG_BASELINE=1`. Match the compiler, SDK/sysroot and feature options
from the current build's CMakeCache. Supply that separate executable as the
second argument to `trig_measure.py`; on Linux prefix Python with `taskset -c 2`.

Raw CSV, corpus, logs, baseline source/build and traces are ignored under
`build/num04-exp/trig-*`. WSL counterparts are under
`/home/alex/photospider-num04-exp-20260924/`. The retained baseline build is a
measurement artifact, not a selectable production backend.

## FreeBSD follow-up (2026-09-25)

A separate [FreeBSD Clang 22 experiment](freebsd-performance.md) passed the same
trigonometric corpora and reports comparable public/core/raw timings plus actual
process instruction/cycle counters. It uses an isolated libc++ 22 runtime because
the base system standard library lacks floating `from_chars`; both compared
builds explicitly use the same compiler and runtime. Earlier macOS/Linux tables
above retain their original compiler/platform scope.
