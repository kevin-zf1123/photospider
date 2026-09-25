# Extended FreeBSD numeric performance analysis, 2026-09-25

Subsequent [FreeBSD/WSL diagnosis](freebsd-wsl-diagnosis.md) found that this
campaign ran with an HWP package limit that restricted the P-core to about
2.922 GHz. A controlled same-binary experiment raised the limit and reached
4.737 GHz, removing most of the historical WSL gap. The measurements below
retain their original machine-state scope; they are not an unconstrained OS
comparison.

The subsequent [adapter and FP64 exp update](adapter-performance.md) implements
the packed-loop optimization and restores certified SLEEF binary64 exp. Its
before/after results use the corrected per-core HWP configuration.

## Scope and conclusions

The agreed scope is this optimization series: exp, supported certified math
batches, six trigonometric kernels, and representative CRV inverse/uniform
lowpass workloads. It is not an inventory or release validation of every NUM/CRV
operator. No production mathematical implementation was changed in this analysis.

The principal findings are:

1. Admitted ordinary inputs retain large public speedups, reproduced with three
   paired A/B rounds and alternating process order.
2. Tiny arrays are dominated by fixed initialization and execution overhead.
   Large arrays are dominated by the generic batch adapter for sin/cos/sinc/exp;
   sincpi spends a materially larger fraction inside its mathematical kernel.
3. A 6.25% strict-fallback fraction almost eliminates the radian sin/cos/sinc
   speedup. The mixed sin case is consistently about 2% slower than the baseline.
4. Accelerated inverse is not uniformly faster: the tested collinear Float32
   inverse workloads are 7–17% slower than Strict. Uniform lowpass varies from
   roughly 5x to 150x faster, depending on kernel and certification behavior.
5. Retired load L3 misses are low in these runs. The evidence favors reducing
   initialization, per-element adapter instructions and certified fallback work
   before replacing another short polynomial solely for throughput.

## Method and coverage

The [initial FreeBSD report](freebsd-performance.md) records the full toolchain
setup: FreeBSD 15.1, i9-12900, explicit Clang 22.1.7 and isolated LLVM libc++ /
libc++abi / libunwind 22.1.7. Both current and `6786cb93` baseline use the same
compiler/runtime/options; system aliases remain unchanged. The baseline has its
own source/build tree and kernel archive.

All timed workloads are pinned with `cpuset -l 2`. No build, sampler or competing
benchmark runs concurrently with the latency measurements. No power policy was
changed. One worker, one warmup, seven timed iterations, result cache disabled,
and complete output checks outside timing are retained. Compile/freeze is outside
public timing. Core means the direct callback including its allocation/gather /
store work; raw means preallocated arithmetic arrays plus scoped fenv. Core/raw
now skip constructing an unused public execution context. Public code and its
timing boundary are unchanged.

The final dataset has **542 primary timing rows**, plus 21 complementary rows,
60 generic-math rows, 62 public unary/binary rows and 36 CRV rows: **721 rows**.
The main sizes are 1, 7, 8, 9, 63, 64, 65, 256, 4,096, 65,536, 262,144,
1,048,576 and 4,194,304. Sub-vector and 64-element batch tails are explicit.
There are three paired ordinary A/B rounds, additional paired mixed-domain
rounds, four direct callback layouts, domain/landmark/tiny-input cases, and
sincpi spans up to 2^24. The exp campaign covers [-10,10], [-80,80] and mixed
subnormal fallback.

The primary trig/exp timings do not enable the optional managed resource ledger.
The public unary/binary and CRV fixtures **do** enable it. Their absolute times
must not be compared as if their accounting paths were identical. Whole
per-value fallback counters remain unavailable (N/A); the specified input
fractions and observed code paths are not fabricated runtime counters.

All arithmetic kernels retain the earlier independent MPFR acceptance. After
the driver changes, six trig corpora and the generic batch fixture passed again.
Float32 inverse checks require exact identity for the independently known,
exactly representable collinear queries. On this host, the focused independent
oracles passed 407 Fraction inverse cases and 474 directed MPFR uniform lowpass
cases for the x86 profile. Every measured case completed its driver result
checks; timing smoke checks themselves are not substitutes for the analytic
certificates or independent oracles.

## Ordinary-input repeatability

N=262,144; values in [-1,1], except sinpi/cospi in [-1/4,1/4]. The middle two
columns are medians of the three process medians, in milliseconds. The last
column gives the range of the three paired ratios, not a confidence interval.

| Function | Baseline ms | Current ms | Median ratio | Paired ratio range |
| --- | --- | --- | --- | --- |
| sin | 30.299 | 2.076 | 14.60x | 14.59–14.61x |
| cos | 27.620 | 2.081 | 13.28x | 13.26–13.32x |
| sinpi | 107.066 | 2.021 | 52.97x | 52.96–61.90x |
| cospi | 105.971 | 2.026 | 52.31x | 52.30–61.19x |
| sinc | 92.819 | 2.055 | 45.17x | 45.15–45.29x |
| sincpi | 128.123 | 2.489 | 51.48x | 51.03–51.49x |

One sinpi/cospi baseline round was about 17% slower than the other two while
current medians remained stable. The table retains that variation; three rounds
are enough to reconfirm large gains, not to justify sub-percent ranking claims.

## Scale, fixed overhead and arithmetic share

Selected current medians, **microseconds**:

| Function | N | Public us | Core us | Raw us |
| --- | --- | --- | --- | --- |
| sin | 1 | 51.649 | 14.841 | 0.194 |
| sincpi | 1 | 51.944 | 14.894 | 0.193 |
| sin | 64 | 51.685 | 15.158 | 0.195 |
| sincpi | 64 | 52.167 | 14.929 | 0.453 |
| sin | 65 | 52.525 | 14.877 | 0.212 |
| sincpi | 65 | 53.018 | 15.242 | 0.381 |
| sin | 4,096 | 83.263 | 40.890 | 0.843 |
| sincpi | 4,096 | 92.483 | 47.405 | 11.192 |
| sin | 1,048,576 | 8017.210 | 7606.230 | 261.193 |
| sincpi | 1,048,576 | 9719.710 | 9235.360 | 2912.050 |
| sin | 4,194,304 | 35499.300 | 31898.300 | 1516.390 |
| sincpi | 4,194,304 | 42172.100 | 38252.800 | 11557.300 |

For N<=65, the public floor is approximately 52 us and core approximately
15 us; changing the tail by one element produces no stable throughput cliff.
The raw microsecond fractions are close to timer/guard overhead and should not
be used for precise small-vector speed comparisons.

At N=1,048,576, sin's raw 0.261 ms is much smaller than its 7.606 ms callback.
Sincpi raw is 2.912 ms versus a 9.235 ms callback. These ratios show the size of
the gap but are not exact additive phase fractions: independent runs have
different allocations/cache effects. Sampled leaf attribution below provides
separate corroboration.

At N=4,194,304, sin public/core/raw is 35.499/31.898/1.516 ms; sincpi is
42.172/38.253/11.557 ms. Whole execution remains one callback; this study does
not establish multi-worker scaling for one operator or multi-node throughput.

## Domain sensitivity and fallback cliffs

The mixed workload changes every sixteenth input. For sin/cos/sinc the replacement
is 2 radians, outside their admitted small interval. For sinpi/cospi it is 0.375,
which retains the existing reduced path; for sincpi it is the exact integer 2,
which its all-finite kernel handles directly. These are different semantic
paths, not one universal fallback percentage.

N=4,096, public medians in milliseconds:

| Function | Current ordinary | Current mixed | Baseline mixed | Mixed speedup |
| --- | --- | --- | --- | --- |
| sin | 0.083 | 60.773 | 59.653 | 0.982x |
| cos | 0.083 | 60.835 | 59.527 | 0.979x |
| sinpi | 0.083 | 0.169 | 1.718 | 10.157x |
| cospi | 0.082 | 0.168 | 1.697 | 10.091x |
| sinc | 0.083 | 66.192 | 65.953 | 0.996x |
| sincpi | 0.092 | 0.091 | 1.954 | 21.415x |

Three extra alternating-order pairs confirm the mixed sin slowdown: medians
59.648 ms baseline versus 60.857 ms current, about **2.03% slower**. Mixed sinc
is 65.920 versus 66.220 ms, about **0.45% slower**. The small regressions are
reported as observations; their precise low-level cause was not isolated. Both
versions spend almost all of these workloads in the same wide-integer strict
refinement, so the normal-domain speedup is absent.

All-outside N=1,024 radian cases cost approximately 244 ms for sin/cos and
265 ms for sinc. For tiny x=2^-130, sin is about 88 ms and sinpi about 49 ms;
cos/cospi/sinc/sincpi still use their stable near-zero paths and are about 61 us.
At the exact pi quarter landmark x=0.25, sinpi/cospi keep algebraic correct
rounding: 4,096 outputs cost about 23 ms on current and 22.6 ms on baseline.
A fast polynomial must not silently replace this exact landmark contract.

Exp has the same domain issue: N=4,097 with every sixteenth input -90 costs
76.449 ms public / 76.008 ms core. These lanes produce subnormal results and
retain the strict engine. Its ordinary 4,096-element fast workload is orders
of magnitude cheaper.

Sincpi has no finite-input strict-refinement cliff, but its arithmetic/control
cost depends on reduction: at N=262,144, public/core/raw ms are
2.255/2.119/0.519 for span 0.25, 2.476/2.315/0.749 for span 1,024, and
3.308/3.126/1.524 for span 2^24. The last range exercises a mixture of integer
bypass and residual reconstruction. The hardware division is retained.

## Layout behavior

Direct workflow declarations require whole dense storage. Attempting to declare
a nonzero-offset or reversed direct binding is rejected before execution; these
are not reported as successful public timings. Computed views and direct
callbacks have different contracts. The direct callback layout experiment
therefore runs only at the core boundary for nonstandard layouts.

N=65,536, current core medians in microseconds:

| Function | Dense | Unaligned +1 byte | Negative stride | Zero stride |
| --- | --- | --- | --- | --- |
| sin | 481.685 | 486.334 | 2108.870 | 1908.640 |
| sincpi | 628.438 | 622.834 | 2257.230 | 2062.840 |

Unaligned contiguous copies are close to aligned throughput. Negative/zero
strides invoke generic per-element address resolution and cost roughly 3–4x
more here. Zero-stride inputs have repeated logical values, so their arithmetic
input distribution differs from the random dense case. The profiler and source
also show generic coordinate updates even when a packed pointer is available.

## Other mathematical calculations

Current Float32 exp medians, microseconds; these are scale measurements of the
already adopted implementation, not an additional exp A/B comparison:

| N | Input interval | Public us | Core us | Raw us |
| --- | --- | --- | --- | --- |
| 4,096 | [-10,10] | 77.525 | 35.582 | 1.200 |
| 4,096 | [-80,80] | 77.142 | 35.540 | 1.504 |
| 262,144 | [-10,10] | 1735.090 | 1608.280 | 83.504 |
| 262,144 | [-80,80] | 1735.980 | 1581.790 | 83.554 |
| 1,048,576 | [-10,10] | 6741.040 | 6306.100 | 298.896 |
| 1,048,576 | [-80,80] | 6716.620 | 6282.150 | 643.244 |

The two large raw span runs differ while their public/core times remain close.
These single-process scale rows do not isolate whether that raw difference is
repeatable or establish its cause. The separately sampled public run supports
the adapter-overhead conclusion below.

The generic batch driver covers ln/sin/cos/tan/pow/atan2, both Float32 and
Float64, at N=1,64,4096,65536,1048576. It compares scalar arithmetic calls with
complete direct callbacks; those are intentionally different boundaries and
are not public before/after ratios. All scalar/batch comparisons passed.

At N=1,048,576, medians in milliseconds (the scalar side already uses the
accelerated certified scalar evaluator):

| Function | F32 scalar / batch callback | F64 scalar / batch callback |
| --- | --- | --- |
| ln | 281.937 / 115.591 | 272.390 / 99.278 |
| sin | 205.701 / 7.533 | 266.531 / 100.853 |
| cos | 205.615 / 7.533 | 267.824 / 100.273 |
| tan | 281.777 / 117.985 | 274.882 / 101.848 |
| pow | 333.462 / 125.265 | 326.392 / 114.022 |
| atan2 | 297.420 / 117.324 | 290.711 / 103.419 |

Public Float64 examples at N=256 (managed ledger enabled) measured ln=85 us,
tan=86 us, pow=102 us, atan2=98 us and atan2pi=127 us. Inputs are fixed by the
existing fixtures: ln(2), tan(1), and binary operands (2,0.3). Float64 exp(1)
measured **16.152 ms** because direct SLEEF exp was removed and the IQK
certificate admits Float32 only. The Float32 exp speedup does not apply there.
Full small-N tables and larger generic-math timings are retained in the CSVs.

## Representative CRV results

Inverse uses 33 collinear knots x=y and 1/256 queries; lowpass uses the existing
radius-2 pulse fixture. The lowpass table observes 256 disjoint outputs from
1,025 inputs, but Whole execution computes the complete 1,025-element result.
Its returned observation bytes must not be confused with retained full-output
storage. Kaiser uses beta=0 in this fixture; this is not a general Kaiser sweep.
All these runs enable managed accounting.

Public medians, milliseconds:

| Operation | Output | Strict | Accelerated | Strict/accelerated |
| --- | --- | --- | --- | --- |
| invert_linear | F32 | 2.648 | 2.841 | 0.93x |
| invert_pchip | F32 | 5.854 | 6.868 | 0.85x |
| hann_sinc | F64 | 250.233 | 51.664 | 4.84x |
| hamming_sinc | F64 | 253.187 | 52.480 | 4.82x |
| blackman_sinc | F64 | 257.233 | 53.409 | 4.82x |
| kaiser_sinc | F64 | 216.901 | 43.247 | 5.02x |
| gaussian | F64 | 679.742 | 4.528 | 150.12x |

Float32 accelerated inverse is about 7.3% slower for linear and 17.3% slower
for PCHIP in this collinear fixture. It attempts interval inversion before exact
resolution. Float64 inverse is nearly unchanged (linear 2.656/2.569 ms,
PCHIP 5.865/5.882 ms Strict/accelerated); the narrow-only fast attempt is absent.
These observations do not establish behavior on arbitrary nonlinear knots.

For uniform lowpass, Gaussian gains about 150x while the windowed sinc kernels
gain roughly 5x. Source inspection explains a relevant difference: constant
zero candidates are rejected by `FastInterval::accepted`, and uniform fast()
passes even a constant zero through that guard. The windowed pulse fixture
contains zero-only effective tap sets; Gaussian has nonzero support there.
The sampled windowed paths retain significant directed arithmetic and managed
work-lock costs. Exact zero/special shortcuts may be optimized only while
preserving positive-normalizer validation, zero-tap omission and signed-zero
semantics. No runtime fallback rate is exposed, so this is a source-supported
explanation rather than a fabricated measured count.

## Sampling, counters and operating-system observations

Nineteen valid process-mode cycle sampling recordings were collected with
`pmcstat -n 1000000 -P cpu_clk_unhalted.thread`, independently of latency runs.
Final traces have 2,878–15,991 displayed samples. Short recordings that produced
no samples were rerun longer. Unknown-function samples are at most 6.4% (the
small public trace); most ordinary large traces are below 1%. Source/PC mapping
uses LLVM 22 addr2line. Sampling skid and inlining prevent interpreting one
source line's percentage as the exact cost of an individual instruction.

Selected **leaf** shares (not additive inclusive stacks):

| Workload | Main observed leaf shares |
| --- | --- |
| sin public, N=262144 | point adapter 71.80%; MutableBuffer::data 8.92%; memcpy 6.10%; trig SIMD 4.29% |
| sincpi public, N=262144 | adapter 44.09%; trig SIMD 31.83%; MutableBuffer::data 8.27% |
| exp public, N=262144 | adapter 64.02%; MutableBuffer::data 11.40%; exp SIMD 7.81% |
| sin baseline core | nextafter 33.51%; dynamic interval certification remains visible |
| sincpi baseline core | nextafter 30.62%; SLEEF adapter 19.84%; exact landmark classification 14.04% |
| mixed sin | DirectedInterval::shift_unsigned 24.88%; compare_keys 14.45%; interval add 8.99% |
| mixed exp | shift_unsigned 28.38%; compare_keys 12.21%; fixed multiplication 11.43% |
| inverse accelerated F32 | ExactCurve::add 27.88%; fixed multiplication 13.52%; exact arithmetic remains dominant |
| lowpass accelerated | mutex unlock/lock and managed work callbacks are prominent alongside directed arithmetic |
| sin core, N=1 | memset 83.79% |
| sin public, N=1 | memset 21.76%; SHA256 block 10.62%; free/malloc 13.58% combined |

The small public SHA256 stacks include dependency observation identities despite
result-cache disablement. This is execution bookkeeping, not a measured cache
hit. For N=1 the large correctly rounded arithmetic workspace is allocated and
zero-initialized even when the lane uses a short polynomial. Source
`BufferAllocator::allocate` value-initializes its byte array; this corroborates
the memset samples.

Source PCs in the large callback concentrate in the gather/scatter loops,
repeated domain checks, packed-pointer address arithmetic, coordinate updates
and buffer/shared-pointer access. Sincpi kernel PCs additionally cover its
central-path classification and final Float64-to-Float32 conversion. See
`source-hotspots.json` for individual PCs/line mappings.

Hardware counts were collected in separate passes. At N=262,144, 100 core
iterations plus warmup, sin current/baseline retired instructions are
3.105/30.531 billion and cycles 0.573/8.750 billion. Sincpi is
3.221/118.495 billion instructions and 0.781/37.642 billion cycles. These cover
the whole process, including startup and the single warmup check.

| Workload | Branch misprediction rate | Retired load L3 misses per 1000 instructions |
| --- | --- | --- |
| sin current, 262144 | 0.159% | 0.00032 |
| sincpi current, 262144 | 0.231% | 0.00031 |
| sin current, 4194304 | 0.168% | 0.00241 |
| sincpi current, 4194304 | 0.260% | 0.00175 |

The ratios combine separate event-group passes with identical workloads; they
are approximate normalized diagnostics. Retired-load misses exclude stores and
some prefetch traffic, so they are not a complete bandwidth measurement. The
low counts, raw/core gap and instruction attribution support prioritizing
adapter overhead, without claiming that every future size is cache-insensitive.

`truss -f -c` on 200 sin repetitions recorded 440 `_umtx_op` calls for public
and one for core/raw. Public includes a worker thread and waits; its accumulated
syscall duration overlaps worker computation and is not extra CPU time to add
to latency. Loader/setup calls dominate the remaining core/raw syscall lists.
Reported syscall error counts are not operator failures; each driver result
check succeeded. No application file I/O occurs in the arithmetic loop.

Separate maximum RSS measurements: sin/sincpi are about 10–11 MiB at N=4096,
30.5 MiB at N=1048576, and 90.7 MiB at N=4194304. RSS includes benchmark inputs,
raw/output-check arrays, code and allocator retention. Managed Payload is
reported separately per timing row; the earlier 768-byte extra scratch result
is unchanged. No leak claim is inferred from RSS; owner-release checks are the
relevant lifetime evidence.

## Prioritized follow-up experiments

These are proposals supported by this analysis, not changes already implemented:

1. **Reduce the packed callback overhead.** Cache source/output base pointers,
   avoid generic coordinate updates when every input is packed, and avoid
   classifying the same lane twice. Relevant code is
   [execute_point_math](../../../plugins/ops/01-numeric/numeric_math_operation.hpp).
   Preserve indexing/admission charges, cancellation and arbitrary-layout paths.
2. **Remove unnecessary heavy initialization for all-fast small arrays.** The
   N=1 memset share makes this a strong target. Defer construction or separate
   scratch storage only after proving resource admission, fallback allocation
   and buffer-initialization contracts; changing a global allocator casually
   would be a wider ownership/behavior change.
3. **Expand proved useful domains and cache exact landmarks.** Radian input 2,
   tiny sine and pi quarter roots dominate their workloads. Any wider reduction
   needs a final-result proof; exact algebraic constants require independent
   correct-rounding verification. Do not relax the four-step/special contract.
4. **Improve CRV certification/charging rather than assume SIMD suffices.**
   Investigate exact constant-zero shortcuts after positive-normalizer proof,
   analytical linear inversion, and bounded work-debit batching. The current
   [budget consume](../../../src/lib/execution/resources.cpp) takes a mutex;
   preserve exact work limits and cancellation responsiveness.
5. **Optimize sincpi's conversion/reconstruction separately.** It has a larger
   kernel share than the other functions. Evaluate vectorized input/output
   conversion and independent-vector scheduling before approximate reciprocal
   schemes that would require a new error proof.
6. **Treat Float64 exp as a separate project.** It needs a Float64-input
   certificate and range handling; narrowing inputs to reuse IQK is not valid.

## Reproduction and boundaries

`examples/numeric_workflow/freebsd_analysis.py CURRENT BASELINE EXP OUTPUT`
reproduces the 542-row campaign under `cpuset -l 2`. The trig driver accepts
`[profile|measure] [normal|mixed|outside|landmark|tiny] [layout]`, where layout
0/1/2/3 means unaligned/reverse/broadcast/dense; non-dense layouts are core-only.
`math_batch time N` selects the generic math size. `signal_benchmark PROFILE
FILTER [float32|float64] [REPETITIONS]` selects inverse dtype and iteration count.

The remote `full/` directory under `/home/alex/photospider-trig-20260925` retains
scripts, CSV, traces, resolved callgraphs, PC maps, counters, truss summaries and
RSS logs. `signal-profiled` retains the signal binary used by those traces;
subsequent changes strengthened only the outside-timing inverse result check.
Local results are under ignored `build/num04-exp/freebsd/full/`.
`freebsd_plot.py DATA_DIR OUTPUT.png` (matplotlib) reproduces the overview PNG
and SVG. The successful complete timing file is `timing-final.csv`; preliminary
partial files and probe traces are not used in the report.

No production numeric code, CPU power policy, global alias, public ABI or
operator contract changed during this analysis. No full CTest/sanitizer/release
matrix, arbitrary nonlinear CRV sweep, wide-radius lowpass sweep, GPU or multi-node
throughput analysis is implied. The user-selected scope and actual numerical /
profiling evidence bound the conclusions above.
