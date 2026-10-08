# NUM-04 Float32 exp SIMD optimization

The performance tables below record the initial A/B experiment. On promotion,
the selectable SLEEF exp comparison paths and the benchmark-only FP32 SLEEF
source adapter were removed. The maintained driver now runs IQK only; the
comparison CSV files remain historical measurements. The initial promotion also
removed the binary64 SLEEF entry. The subsequent
[adapter/FP64 update](adapter-performance.md) restores certified SLEEF binary64
exp and NUM-01 AST exp, while Float32 NUM-04 retains IQK. No Float64 input is
narrowed to fit the Float32 polynomial. The current Result computation reserves
an arithmetic object, a math batch workspace, and a Float32 SIMD workspace. The
768-byte Float32-only workspace describes the historical initial adapter.

The initial A/B experiment measured the Float32 promotion described here.
Specification status remains Proposed. The maintained
`numeric.exp_accelerated_apple_silicon` and `numeric.exp_accelerated_x86_64`
callbacks now use a 64-sample batch and an IQK-derived normal-range SIMD expf.
Strict and other NUM-04/05 functions retain their arithmetic paths. The initial
A/B experiment retained Float64 SLEEF exp; its later removal and restoration are
separate implementation changes, not additional measurements in these tables.

## Algorithm and correctness boundary

The polynomial is adapted from `ik_llama.cpp`'s `iqk_utils.h`, which attributes
the routine to Justine Tunney and Arm Limited. The original
MIT notice is retained in `third_party/IK_LLAMA_LICENSE.txt` and installed with
the kernel's licenses. Only the normal-range exp polynomial was extracted;
Photospider owns classification and fallback. The reviewed CPU files
`iqk_utils.h`, `iqk_cpu_ops.cpp`, `ggml-impl.h` and `ggml.c` do not provide a
corresponding standalone SIMD sin/pow candidate; relevant ggml CPU call sites
use `sinf`/`powf`. The header's tanh/silu/gelu wrappers are not NUM-04 functions
and were not added to this operator family.

Float32 finite inputs in [-80,80] use explicit FMA range reduction and a degree-5
polynomial, four lanes on NEON or eight on AVX2/FMA. A bit-level input mask
substitutes zero before SIMD arithmetic for every unsupported lane. Those lanes
then execute the existing certified path with original input bits. Zero returns
exactly one; NaN sign/payload, infinities, and subnormal/overflow output cases
retain existing exact handling. Tail lanes duplicate a valid sanitized operand;
all widths/layouts use the same per-lane FMA graph. No global fast-math flag is
introduced and no Float64 argument is narrowed.

`oracle/ops/numeric/exp_bound.py` is a deterministic exact-rational
certificate, not a random accuracy test. It encloses ln(2) with an atanh series,
proves the first reduction FMA exact via Sterbenz, bounds the second FMA, bounds
P-exp using translated Taylor polynomials over 256 complete intervals, and
propagates every binary32 rounding error. Separate negative/positive reduced
intervals and a neighborhood of zero handle the binade crossing. Conservative
reference-distance bounds are <3.123, <2.685 and <3.040 minimum representable
steps respectively. Scaling remains normal for the admitted integer exponents.
This establishes the existing four-step Float32 contract without a per-sample
SLEEF certificate or an unproved reliance on upstream's error comment.

The Result computation phase establishes nearest/gradual floating mode on its
CPU worker and restores that worker's prior environment on exit. The caller
thread's floating-point environment is checked separately around `execute`.
The historical initial adapter used a 1,792-byte fixed batch workspace; that
size does not describe the current Result implementation.
Per-lane indexing and mathematical work are still charged; checks are batched
in groups of at most 64. Fallback owns its own admission/refinement charge,
without double charging. Cancellation and failed work/capacity admission never
publish a partial result. The operation executes as a Whole Result computation
and retains source/build cache identity.

## Result-path acceptance

The current benchmark acceptance runs the production IQK kernel through a
Result workflow. The exp corpus completed with 20,503 inputs across six
partitions, a maximum distance of two Float32 steps, and passing layout,
floating-environment, resource, and cancellation checks. These correctness
results validate the current Result execution route; they are not a new large
performance campaign.

The focused unary, binary, comparisons, and ColorArray Result CTests passed 4/4.
Run them with:

```sh
ctest --test-dir build/kernel-dev -R '^(test_numeric_unary_result|test_numeric_binary_result|test_numeric_comparisons_result|test_numeric_color_array_result)$' --output-on-failure
```

The current exp and trigonometric timing smoke set completed 21 public/core/raw
runs, including unaligned public input, reverse core input, and a 65-sample tail.
CSV output checks confirmed scope labels for the sampled workloads. These are
execution and metadata checks, not a performance comparison or platform matrix.

## Historical validation record

The measurements and validation results in this section record the earlier
Value/callback implementation. They describe that implementation's evidence,
not the current Result workflow or computation-poll timing scopes.

Both macOS NEON and Linux AVX2 passed:

- 20,503 deterministic independent directed-MPFR exp references, including
  ordinary random values, raw random bit patterns, mixed NaNs/Inf/zeros,
  [-80,80] neighbors, and output underflow/normal/overflow boundary neighbors.
  Each corpus ran with chunk lengths 1,3,7,64,65,257 and identical output bits
  across partitions. Maximum observed distance was two Float32 steps.
- Unaligned storage, negative and zero strides, four host rounding modes with
  preexisting exception flags, work/capacity rejection, cancellation during
  arithmetic and unpublished-output release. Mixed fallback work totals and
  rejection thresholds match the previous scalar engine.
- The existing public unary oracle: 7,524 integer/exact/MPFR cases per platform's
  accelerated profile, including Float64 and the other NUM-04 functions.
- Focused `test_numeric_operations` CTest.

MPFR versions: macOS 4.2.2; Linux 4.2.1. MPFR is only a validation dependency.
ClangFormat 21, cpplint and diff whitespace checks passed for the changed C++.
Independent read-only code/spec review found and verified the correction of
fallback admission double charging; final review had no blocker/required finding.

The existing complete unary manual executable passed its arithmetic and
layout/fenv groups, then stopped at a typed image fixture with
`image declaration requires planar layout`. That fixture is outside this exp
change and remains unresolved; the complete manual suite is not reported as
passing. The new exp checks and public oracle above ran independently. No
release-wide CTest, sanitizer, installed-consumer or AVX-512 claim is made.

## Measurement method

macOS: Apple M5, arm64, macOS 27.2 (26B5091g), Homebrew Clang 21.1.3.
Linux: Intel Core i9-12900, Ubuntu WSL2 x86_64, kernel
5.15.167.4-microsoft-standard-WSL2, Ubuntu Clang 18.1.3, process pinned to vCPU 2.
The Linux host exposes 24 logical CPUs; no claim is made about Windows mapping
that virtual CPU to a particular P/E core. macOS has no hard CPU affinity here.
The two hosts are separate measurements, not an ISA-only comparison.

Both historical builds use RelWithDebInfo (-O2 -g); the IQK and benchmark-only
FP32 SLEEF translation units use -O3, no fast-math and disabled implicit FMA
contraction. The explicit FMA intrinsics remain fused. The comparison used
SLEEF 3.9.0. The FP32 comparison directly included its unmodified source, with
math inlined in the batch loop, as confirmed by the absence of a separate expf
symbol in the macOS object.

The historical A/B measurements use seed 404, prebuilt inputs and reused input
buffers, one warmup, seven timed calls, one public CPU worker, and result cache
off. Every timed result is checked against an independent double-libm smoke
expectation outside timing; the MPFR acceptance above supplies the stronger
reference. Backend order is fixed and each row uses a separate process. The
historical public path uses Value inputs and its then-current callback-based
workflow. Its `core` layer times a complete direct callback, including callback
allocation and gather, while excluding executor and typed input admission. Its
`raw` layer reuses preallocated math arrays and includes a scoped floating-point
environment guard. These boundaries explain the historical rows only.

- `scalar`: the original per-element CertifiedMath/SLEEF FP64 path, with no
  added batch workspace. It was selected through the private comparison
  factory during this experiment; that factory no longer has a backend switch.
- `sleef`: the same new batch adapter using FP64 SLEEF plus Float32 rounding.
  This separates callback batching/fenv/certificate overhead from IQK selection.
- `iqk`: the production Float32 batch path.
- `sleef32` (raw only): source-inlined FP32 SLEEF u10, a same-width math baseline.

The current `exp_benchmark.cpp` keeps that raw SIMD kernel unchanged and runs
its `public` and `core` layers through a Result workflow. The fixture constructs
immutable `Value` backing, publishes it as a source tensor Result under the
execution Root, compiles a plan for each input schema, and freezes a new binding
for each source Result. Static preparation is reused with the compiled plan.
Cache is off and the workflow uses one CPU worker.

Current `public` timing surrounds `Workflow::run`. It includes the
`ExecutionContext::execute` call, coordinator work, continuation factory and
initial Need, digest calculation, host publication, and the small wrapper that
reads Root statistics and extracts the `ResultRef`. Benchmark operation
definition creation and registration, source construction/publication, compile
and static preparation, freeze, and output readback are outside this timer.
Current `core` timing surrounds only `ResultContinuation::poll` on a CPU worker
phase with supplied tensors. It includes the checked `consume_work` observer
and Result publication. It excludes continuation factory setup, initial Need,
coordinator work, source admission, compile/freeze, and readback. Current `raw`
still times the preallocated SIMD computation with its floating-point
environment scope.
These current scopes are not directly comparable to the historical
Value/callback core timing above.

Current public CSV rows report peak Root Payload, which includes Root-owned
state, scratch, and output. Immutable caller input storage is accounted as
Referenced. This Root accounting is not process RSS. Core and raw report `N/A`
for the Payload peak; that field means unmeasured, not zero allocation. The
historical controlled Payload values and RSS readings below retain their
original accounting boundaries.

## Historical public execution results

Median milliseconds; input is uniform Float32 [-10,10].

| Platform | N | Previous scalar | Batch SLEEF | Batch IQK | Scalar / IQK | Batch SLEEF / IQK |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| M5 / NEON | 16,384 | 1.385 | 0.125 | 0.086 | 16.17x | 1.46x |
| M5 / NEON | 262,144 | 21.394 | 1.222 | 0.663 | 32.26x | 1.84x |
| M5 / NEON | 4,194,304 | 340.127 | 20.529 | 11.307 | 30.08x | 1.82x |
| i9 / AVX2 | 16,384 | 3.668 | 0.122 | 0.094 | 39.01x | 1.30x |
| i9 / AVX2 | 262,144 | 57.173 | 1.312 | 0.911 | 62.79x | 1.44x |
| i9 / AVX2 | 4,194,304 | 916.449 | 22.030 | 16.038 | 57.14x | 1.37x |

For N=262,144, the direct callback medians in milliseconds were:

| Platform | Scalar callback | Batch SLEEF callback | IQK callback |
| --- | ---: | ---: | ---: |
| M5 / NEON | 21.108 | 1.128 | 0.595 |
| i9 / AVX2 | 56.728 | 1.208 | 0.831 |

Public and callback values are independently measured layers. Their difference
is not a measured decomposition of executor overhead. Small inputs remain
scheduler/allocation dominated: N=256 IQK measured about 42 us on both hosts,
and was slightly slower than batch SLEEF in this run. No stable small-N win over
batch SLEEF is claimed.

## Historical raw math and input-range limits

Nanoseconds per element, uniform [-10,10]:

| Platform | N | SLEEF FP64 + RN32 | Inlined SLEEF FP32 | IQK FP32 | SLEEF FP32 / IQK |
| --- | ---: | ---: | ---: | ---: | ---: |
| M5 / NEON | 262,144 | 2.141 | 0.438 | 0.195 | 2.25x |
| M5 / NEON | 4,194,304 | 2.175 | 0.442 | 0.193 | 2.29x |
| i9 / AVX2 | 262,144 | 1.532 | 0.340 | 0.185 | 1.83x |
| i9 / AVX2 | 4,194,304 | 2.127 | 0.457 | 0.373 | 1.22x |

The like-width FP32 math gains are much smaller than the public scalar-to-batch
gains. For the large AVX2 array the FP32 math gain shrinks to about 1.22x;
no hardware-counter evidence is available to assign this to memory bandwidth.

Uniform [-80,80] was also timed with identical validation. At N=4,194,304:

| Platform | Previous scalar ms | Batch SLEEF ms | IQK ms |
| --- | ---: | ---: | ---: |
| M5 / NEON | 347.560 | 19.257 | 11.080 |
| i9 / AVX2 | 916.461 | 21.642 | 16.227 |

The mixed workload replaces every 16th ordinary input with -90. That value
requires exact subnormal output and exercises production strict fallback.
At N=4,097, macOS scalar/IQK measured 41.23/42.46 ms and Linux 37.18/36.18 ms.
There is no robust improvement: refinement dominates, and no underflow semantics
were weakened to improve this result. Pure IQK raw timings intentionally exclude
unsupported input ranges; they do not represent full-domain exp throughput.

Historical public controlled peak Payload at N=4,194,304 is 33,762,640 bytes
for scalar and 33,764,432 bytes for either batch adapter, a 1,792-byte scratch
increase. This metric is not RSS. A separate `/usr/bin/time` run of IQK public
N=4,194,304 reported peak RSS 159,809,536 bytes on macOS and 170,664 KiB on
Linux. These are whole benchmark process measurements, not kernel-only memory.
Benchmark RSS also includes validation vectors, preallocated raw arrays,
compiler/executor state, and both comparison backends.

## Profiler interpretation

Two separate eight-second Instruments Time Profiler recordings used public
N=262,144 [-10,10], checking the first output only in profiling mode. All timing
numbers above come from completed, fully checked runs without the profiler.
Recordings completed, saved and exported successfully; xctrace stopped their
long-running targets at its time limit.

Of 7,890 scalar execution-chain samples, leaf `fesetenv` accounted for 58.09%
and `fegetenv` 10.16%; SLEEF adapter leaves were 3.36%, nextafter 3.70%.
The original whole callback still entered floating mode and constructed a
binary64 acceptance interval for every element. Thus its large speedup must
not be attributed entirely to a faster exp polynomial.

Of 7,598 IQK execution-chain samples, 44.29% were in the point-math callback
itself, 14.95% in shared_ptr::get, 9.61% in MutableBuffer::data, and 7.11% in
vector::size leaves. The SIMD adapter and inlined polynomial together accounted
for 9.24% inclusive. There was one fegetenv-inclusive sample and no fesetenv,
nextafter or SLEEF samples. Input gathering/output access now occupy a much
larger share of this shortened callback. Inclusive categories overlap and are
not summed as disjoint costs.

WSL `perf_event_open` for CPU cycles returned EPERM (Permission denied), and no
perf executable was available. Linux measurements therefore establish latency
and throughput only, not cycle, cache-miss or instruction-count explanations.

## Reproduction and artifacts

```sh
mkdir -p build/num04-exp
cmake --build build/kernel-dev --target photospider_numeric_exp_benchmark -j8
python3 oracle/ops/numeric/exp_bound.py
python3 oracle/ops/numeric/exp_oracle.py build/num04-exp/oracle.bin
build/kernel-dev/examples/numeric_workflow/photospider_numeric_exp_benchmark check build/num04-exp/oracle.bin
build/kernel-dev/examples/numeric_workflow/photospider_numeric_exp_benchmark --timing-scopes
python3 examples/numeric_workflow/exp_measure.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_exp_benchmark build/num04-exp/result.csv
```

Append Linux CPU id `2` to `exp_measure.py` to pin each process to vCPU 2. Python
must load MPFR 4.2+; the current driver measures the production implementation
only and has no SLEEF comparison backend. For one run, use
`photospider_numeric_exp_benchmark public 262144 10 7`. The normal formal exp
helper selects the production path for its matching accelerated profile. The
historical A/B tables are not reproduced by this current command because their
Value/callback timing boundaries differ from the Result workflow and
computation-poll scopes.

Raw local files: `build/num04-exp/{mac,linux}.csv`, MPFR corpus, oracle/check/CTest
logs, `scalar.trace`, `iqk.trace`, their exported sample XML and
`profile-summary.txt`. WSL source/results are at
`/home/alex/photospider-num04-exp-20260924`. These are ignored measurement outputs;
reusable drivers, the proof and implementation are tracked-source changes.

## Historical promotion validation

After removal of all direct SLEEF exp calls, the final macOS and WSL builds
re-ran the 20,503-case exp corpus (including scalar/Whole bit identity), the
7,524-case public unary oracle, and the 715-case NUM-01 Fraction/MPFR expression
oracle. The numeric and expression focused CTests passed on both platforms.
The retained Float32 kernel has the same coefficient/FMA graph as the measured
candidate; historical A/B data above are not new Float64 or expression timings.
At that promotion, the Float32-only batch workspace was 768 bytes. No installed
public API or ABI was added. SLEEF remains required for the other mathematical
functions.
