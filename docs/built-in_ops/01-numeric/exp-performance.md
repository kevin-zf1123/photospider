# NUM-04 Float32 exp SIMD optimization

The performance tables below record the initial A/B experiment. On promotion,
the selectable SLEEF exp comparison paths and the benchmark-only FP32 SLEEF
source adapter were removed. The maintained driver now runs IQK only; the
comparison CSV files remain historical measurements. The SLEEF exp adapter
entry was also removed: Float64 exp and NUM-01 AST exp now use strict replay.
Those workloads can be substantially slower; no Float64 input is narrowed to
fit the Float32 polynomial. Removing comparison-only
arrays reduces the production batch workspace from 1,792 to 768 bytes.

Measured 2026-09-24 from `ops-impl` based on `4138e804`, with the local changes
in this report. Specification status remains Proposed. The maintained
`numeric.exp_accelerated_apple_silicon` and `numeric.exp_accelerated_x86_64`
callbacks now use a 64-sample batch and an IQK-derived normal-range SIMD expf.
Strict and other NUM-04/05 functions retain their arithmetic paths. The initial
A/B experiment retained Float64 SLEEF exp; final promotion removes it as stated
above.

## Algorithm and correctness boundary

The polynomial is adapted from
[ik_llama.cpp iqk_utils.h at dad2cb3](https://github.com/ikawrakow/ik_llama.cpp/blob/dad2cb3e55138cbdd7df988c66c0c3c54f6d34ad/ggml/src/iqk/iqk_utils.h),
which attributes the routine to Justine Tunney and Arm Limited. The original
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

`examples/numeric_workflow/exp_bound.py` is a deterministic exact-rational
certificate, not a random accuracy test. It encloses ln(2) with an atanh series,
proves the first reduction FMA exact via Sterbenz, bounds the second FMA, bounds
P-exp using translated Taylor polynomials over 256 complete intervals, and
propagates every binary32 rounding error. Separate negative/positive reduced
intervals and a neighborhood of zero handle the binade crossing. Conservative
reference-distance bounds are <3.123, <2.685 and <3.040 minimum representable
steps respectively. Scaling remains normal for the admitted integer exponents.
This establishes the existing four-step Float32 contract without a per-sample
SLEEF certificate or an unproved reliance on upstream's error comment.

The callback borrows nearest/gradual floating mode once per batch-enabled
invocation and restores caller state on every exit. The 1,792-byte fixed batch
workspace is admitted with the arithmetic workspace for accelerated exp.
Per-lane indexing and mathematical work are still charged; checks are batched
in groups of at most 64. Fallback owns its own admission/refinement charge,
without double charging. Cancellation and failed work/capacity admission never
publish a partial result. Whole execution and source/build cache identity remain
in effect; per-value fallback diagnostics remain N/A.

## Validation

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

Both builds use RelWithDebInfo (-O2 -g); the IQK and benchmark-only FP32 SLEEF
translation units use -O3, no fast-math and disabled implicit FMA contraction.
The explicit FMA intrinsics remain fused. SLEEF is pinned to 3.9.0, commit
906ca7512ee483296780a81a21b9ca715d40dfe1. The FP32 comparison directly includes
its unmodified source, with math inlined in the batch loop, as confirmed by the
absence of a separate expf symbol in the macOS object.

All measurements use seed 404, prebuilt inputs and reused input buffers, one
warmup, seven timed calls, one public CPU worker and result cache off. Every
timed result is checked against an independent double-libm smoke expectation
outside timing; the MPFR acceptance above supplies the stronger reference.
Backend order is fixed and each row uses a separate process. Small-N variation
and cross-process noise do not support claims of small speed differences.

- `scalar`: the original per-element CertifiedMath/SLEEF FP64 path, with no
  added batch workspace. It was selected through the private comparison
  factory during this experiment; that factory no longer has a backend switch.
- `sleef`: the same new batch adapter using FP64 SLEEF plus Float32 rounding.
  This separates callback batching/fenv/certificate overhead from IQK selection.
- `iqk`: the production Float32 batch path.
- `sleef32` (raw only): source-inlined FP32 SLEEF u10, a same-width math baseline.

`public` uses a private comparison key authored in a WorkflowDocument and the
public Compiler/freeze/execute_fragments route. It includes dispatch, collect,
output allocation and callback; compile/freeze and verification are outside the
timer. The formal keys are separately covered by the public unary oracle and
resource tests. Payload ceiling is 1 GiB; discovery/Footprint work limits are
2^50, cache proof work is zero. The optional managed resource ledger is unset in
these timed public runs; separate managed-budget tests exercise accounting.
`core` means the complete direct callback, including its allocations and gather,
but excluding executor and typed input admission. `raw` reuses preallocated math
arrays and includes a scoped fenv guard. Raw FP64 SLEEF includes output narrowing;
its input widening is outside timing. Core/raw managed peaks are unmeasured
(N/A); initial CSV files use zero as the unavailable-field sentinel, not as a
zero-allocation assertion.

## Public execution results

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

## Raw math and input-range limits

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

Public controlled peak Payload at N=4,194,304 is 33,762,640 bytes for scalar and
33,764,432 bytes for either batch adapter, a 1,792-byte scratch increase. This
metric is not RSS. A separate `/usr/bin/time` run of IQK public N=4,194,304 reported peak RSS
159,809,536 bytes on macOS and 170,664 KiB on Linux. These are whole benchmark
process measurements, not kernel-only memory. Benchmark RSS also includes validation vectors,
preallocated raw arrays, compiler/executor state and both comparison backends.

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
cmake --build build --target photospider_numeric_exp_benchmark photospider_numeric_unary test_numeric_operations -j8
python3 examples/numeric_workflow/exp_bound.py
python3 examples/numeric_workflow/exp_oracle.py build/num04-exp/oracle.bin
build/examples/numeric_workflow/photospider_numeric_exp_benchmark check build/num04-exp/oracle.bin
python3 examples/numeric_workflow/unary_oracle.py build/examples/numeric_workflow/photospider_numeric_unary apple
ctest --test-dir build -R '^test_numeric_operations$' --output-on-failure
python3 examples/numeric_workflow/exp_measure.py build/examples/numeric_workflow/photospider_numeric_exp_benchmark build/num04-exp/mac.csv
```

Use `x86` for the Linux unary oracle and append `2` to exp_measure.py to pin it
to Linux vCPU 2. Python must be able to load MPFR 4.2+; on this Mac it was
`/opt/homebrew/bin/python3.11`. Create the output directory before corpus generation.
The retained driver measures only the production implementation, with no SLEEF
backend option. For a single run use `photospider_numeric_exp_benchmark public
262144 10 7`. The normal formal exp helper automatically selects the new path on
its matching accelerated profile. Reproducing the removed alternatives requires
the experimental revision; the historical tables do not describe an available
production switch.

Raw local files: `build/num04-exp/{mac,linux}.csv`, MPFR corpus, oracle/check/CTest
logs, `scalar.trace`, `iqk.trace`, their exported sample XML and
`profile-summary.txt`. WSL source/results are at
`/home/alex/photospider-num04-exp-20260924`. These are ignored measurement outputs;
reusable drivers, the proof and implementation are tracked-source changes.

## Promotion validation

After removal of all direct SLEEF exp calls, the final macOS and WSL builds
re-ran the 20,503-case exp corpus (including scalar/Whole bit identity), the
7,524-case public unary oracle, and the 715-case NUM-01 Fraction/MPFR expression
oracle. The numeric and expression focused CTests passed on both platforms.
The retained Float32 kernel has the same coefficient/FMA graph as the measured
candidate; historical A/B data above are not new Float64 or expression timings.
The production batch workspace is now 768 bytes. No installed public API or ABI
was added. SLEEF remains required for the other mathematical functions.
