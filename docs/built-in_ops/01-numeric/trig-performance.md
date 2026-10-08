# Certified SIMD trigonometric kernels

The SIMD implementation accelerates the existing Float32 NUM-04 profiles on
macOS ARM NEON and Linux x86 AVX2/FMA.
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

Run `python3 oracle/ops/numeric/trig_bound.py`. It reads the actual C++
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

## Historical performance measurements

The table records a pre-Result A/B run. Its baseline and new executable used
their own matching source and test-kernel builds and the same Value-based
WorkflowDocument fixture. The baseline disabled the unavailable raw polynomial
mode. These measurements retain their original implementation and timing
boundaries; they are historical records rather than a current Result-path
performance comparison.

Hosts: Apple M5, macOS 27.2, Homebrew Clang 21.1.3, macOS 27.0 SDK in both
compared builds; Intel i9-12900, Ubuntu WSL kernel 5.15.167.4, Clang 18.1.3,
AVX2/FMA, pinned to CPU 2. Builds use RelWithDebInfo (-O2 -g), the private SIMD
object uses -O3, and fast-math remains disabled. An initial macOS baseline
selected a different SDK; it was rebuilt with the current sysroot and the
complete macOS timing sequence was rerun for the table below.

One warmup plus seven measured samples, medians, seed 404. Historical public
runs use one CPU worker, result cache off, a 1 GiB public live-byte ceiling,
dependency work limits 2^50 and zero dependency cache-proof work. Compile/freeze
and complete output smoke checks are outside timing. Input is uniform in
[-1,1], except sinpi/cospi in [-1/4,1/4]. Historical public includes
execute/collect; historical core includes callback allocation, gather, and
store; raw includes only preallocated SIMD arrays and the environment scope.
Profiler recordings are separate from timing.

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

At N=1,048,576, historical medians for the new raw kernel were:

| Function | M5 raw ms | Linux raw ms |
| --- | ---: | ---: |
| sin | 0.182 | 0.205 |
| cos | 0.186 | 0.188 |
| sinpi | 0.182 | 0.214 |
| cospi | 0.185 | 0.201 |
| sinc | 0.191 | 0.177 |
| sincpi | 2.175 | 1.825 |

The historical raw/core/public CSV also covers N=4,096 and 262,144. Fixed
baseline-first process order, WSL scheduling variance, and one timing sequence
per host limit small-difference claims. Speedups describe these admitted
ordinary workloads; they are not full-domain or all-CRV performance claims. Raw
has no old-backend comparison and cannot isolate the polynomial's cost relative
to SLEEF alone.

The gain combines SIMD batching, narrower arithmetic for five kernels and
removal of per-element dynamic interval certification in favor of the proved
static domain. Sinc additionally avoids division, and sincpi avoids repeated
sine/cosine enclosures and division interval propagation.

## Historical memory and profiling

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

## Current Result-path acceptance

The current benchmark runs the production kernels through a Result workflow.
Each corpus completed across six partitions and passed layout,
floating-environment, resource, and cancellation checks on the tested host and
profile. These are correctness results, not a new performance campaign or a
multi-platform timing claim.

| Function | Corpus cases per partition | Partitions | Maximum observed distance |
| --- | ---: | ---: | ---: |
| sin | 4,742 | 6 | 1 step |
| cos | 4,742 | 6 | 1 step |
| sinpi | 4,742 | 6 | 2 steps |
| cospi | 4,742 | 6 | 1 step |
| sinc | 4,742 | 6 | 1 step |
| sincpi | 10,333 | 6 | 0 steps |

The production exp kernel also passed 20,503 cases across six partitions with a
maximum distance of two steps; its layout, floating-environment, resource, and
cancellation checks passed. A zero observed distance is a corpus result, not a
claim of correct rounding over the full input domain.

The current timing smoke set completed 21 public/core/raw runs across exp and
the six trigonometric functions, covering unaligned public input, reverse core
input, and a 65-sample tail. The three CSV drivers also emitted valid timing
scope fields for their checked workloads. These checks establish that the
current timers execute and label their rows; they are not a performance
regression campaign or a platform matrix.

## Historical validation and reproduction

The validation results below record the earlier implementation and its
platform runs. They remain useful for the historical tables and analytic
certificate, but do not establish a current platform-wide performance result.

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
cmake -S . -B build/kernel-dev
cmake --build build/kernel-dev --target photospider_numeric_trig_benchmark -j 8
python3 oracle/ops/numeric/trig_bound.py
python3 oracle/ops/numeric/trig_oracle.py build/trig-corpus
for f in sin cos sinpi cospi sinc sincpi; do
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_trig_benchmark \
    check "$f" "build/trig-corpus/$f.bin"
done
build/kernel-dev/examples/numeric_workflow/photospider_numeric_trig_benchmark --timing-scopes
python3 examples/numeric_workflow/trig_measure.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_trig_benchmark
```

The current executable reports `public=result_workflow`,
`core=result_computation_poll`, and `raw=simd_kernel` from `--timing-scopes`.
`trig_measure.py` records each executable's declaration in the `timing_scope`
column. An older executable that lacks this query is marked `unknown`; rows with
unknown or different scopes must not be treated as comparable. In particular,
the current computation-poll core timer cannot be compared directly with the
historical direct-callback core rows above. To add a current core measurement
for a specific workload, run the benchmark directly, for example:

```sh
build/kernel-dev/examples/numeric_workflow/photospider_numeric_trig_benchmark \
  sin core 1048576 1 7 measure normal 3
```

Current public and core runs publish source `Value` backing as a Result under
the execution Root, reuse the compiled plan and static preparation for the same
input schema, and freeze a fresh binding for each source Result. Cache is off
and one CPU worker is used. The public timer surrounds `Workflow::run`. It
includes its `ExecutionContext::execute` call, coordinator work, continuation
factory and initial Need, digest calculation, host publication, and the small
wrapper that reads Root statistics and extracts the `ResultRef`. Benchmark
operation definition creation and registration, source construction/publication,
compile and static preparation, freeze, and readback are outside it. The core
timer measures only `ResultContinuation::poll` for a worker phase with supplied
tensors, including the checked `consume_work` observer and Result publication.
It excludes continuation factory setup, initial Need, coordinator work, source
admission, compile/freeze, and readback. Raw retains its SIMD timer. Public peak
Payload is the Root's managed Payload peak, including Root-owned state, scratch,
and output; immutable caller input storage is accounted as Referenced. It is not
RSS. Core and raw peak values are `N/A`, meaning unmeasured.

An old baseline can still be passed to `trig_measure.py` to record its rows, but
its legacy `core` scope is not a paired current-core measurement. Do not infer a
speedup from rows whose `timing_scope` values differ or are `unknown`. No new
large timing campaign is reported here. On Linux prefix the measurement script
with `taskset -c 2` when CPU affinity is needed.

Raw CSV, corpus, logs, baseline source/build and traces are ignored under
`build/num04-exp/trig-*`. WSL counterparts are under
`/home/alex/photospider-num04-exp-20260924/`. The retained baseline build is a
measurement artifact, not a selectable production backend.

## FreeBSD follow-up

A separate [FreeBSD Clang 22 experiment](freebsd-performance.md) records another
platform's historical trigonometric timings and process instruction/cycle
counters. It uses an isolated libc++ 22 runtime because the base system standard
library lacks floating `from_chars`; its reported timer scopes should be checked
before comparing rows with current Result measurements. The macOS/Linux tables
above retain their original compiler/platform scope.
