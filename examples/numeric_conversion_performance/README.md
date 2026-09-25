# FMT-06 numeric conversion workflow and performance

`main.cpp` runs the public `WorkflowDocument` → `Compiler` →
`ExecutionContext` path with a tiled planar `[size,size,4]` input. It checks
registry, compile and execution failures. The deterministic input code is
`(linear_pixel*13 + channel*29) % 256`. The integration test
`test_numeric_conversion` independently checks conversion bytes, metadata,
regions, 49 dtype endpoint pairs, NaN bits and rounding boundaries.

Build and run:

```sh
cmake --build build --target photospider_numeric_conversion_performance test_numeric_conversion -j 8
ctest --test-dir build -R '^test_numeric_conversion$' --output-on-failure
build/examples/numeric_conversion_performance/photospider_numeric_conversion_performance 4096 u8-f32 full 3
```

The positional arguments are `size`, `pair` (`u8-f32`, `f32-u8`, `f64-f32`,
`i64-u8`), `coverage` (`full`, `channel`, `tile`), repetitions, and optional
`tile_extent` (`128`, the default, or `256`). `channel`
requests one complete plane. `tile` requests `[127,130) × [127,130)` on
channel 1, crossing both tile boundaries at the default tile size. With 256
tiles, the ROI is `[255,258) × [255,258)`; size must cover that ROI. For `u8-f32`, code 0 maps to 0,
128 maps to correctly rounded binary32 `128/255`, and 255 maps to 1. The
unrequested channels have no conversion demand.

## NEON measurements before whole-tile SME on 2026-09-24

MacBook Pro, Apple M5, 32 GiB RAM, macOS 27.2, Clang 21.1.3,
RelWithDebInfo, CPU strict profile, one worker, tiled input. To reproduce this
NEON stage, configure `-DPHOTOSPIDER_ENABLE_NUMERIC_CONVERSION_SME=OFF`. Inputs are fully
published before timing. Each command uses a fresh process; `first_ms` is the
first execution and `repeat_ms` is the arithmetic mean of two later executions
on the same context. Result caching was disabled. Neither setup nor compilation
is included in execution times.

The only registration is `numeric.convert_format_strict`. The implementation
selects AArch64 NEON or runtime-checked amd64 AVX2 for eligible contiguous
planar spans. amd64 without AVX2 retains the portable scalar path. SIMD must
produce the strict result: UInt8 expansion uses division by 255 (multiplication
by a rounded reciprocal is not equivalent), Float32 compression multiplies
exactly in binary64 before ties-even rounding, and Float64 narrowing falls back
to bit conversion for zero, subnormals, nonfinite values and overflow. Int64
full-range mapping uses an exact 128-bit numerator and division by `2^64-1`
reduced to high/low additions and a comparison. Other interval maps retain
exact rational evaluation. Work/cancellation checks remain at 64-sample bounds in these span paths.

| Pair | Previous scalar full repeat | Current full first | Current full repeat | Speedup | One channel repeat | Cross-tile 3×3 repeat |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| UInt8→Float32 | 358.1 ms | 23.36 ms | 21.94 ms | 16.3× | 5.46 ms | 0.039 ms |
| Float32→UInt8 | 457.2 ms | 43.61 ms | 35.85 ms | 12.8× | 8.67 ms | 0.032 ms |
| Float64→Float32 | 486.9 ms | 71.37 ms | 59.05 ms | 8.2× | 13.99 ms | 0.034 ms |
| Int64→UInt8 | 327.6 ms | 42.03 ms | 35.45 ms | 9.2× | 7.97 ms | 0.065 ms |

All table runs use 4096²×4 logical inputs; full, channel and tile are separate
processes. The scalar column is the earlier same-day implementation measured
with the same workload/settings; it predates span kernels and width hoisting.
These are before/after observations, not a same-binary A/B harness. The current
commands ran serially without a concurrent build. Sub-millisecond ROI timings
are especially sensitive to scheduling. No performance guarantee is implied.

Before the initial scalar specialization, the all-rational 256²×4 repeat times
were 470.5, 464.2, 841.6 and 871.2 ms in table order; scalar specialization reduced
them to 1.60, 1.90, 2.14 and 1.51 ms. Those smaller measurements describe the
initial implementation stage and are not used in the SIMD speedup ratios.

At 4096²×4 full coverage, `source_backed` is 64 MiB for UInt8, 256 MiB for
Float32, and 512 MiB for Float64/Int64. `output_backed` is 256 MiB for Float32
and 64 MiB for UInt8. `output_reserved` equals full output logical bytes even
for ROI; only requested pages are backed. The diagnostic `source_read_bytes`
equals exact requested input bytes: for the 3×3 single-plane ROI it is 9,
36 or 72 bytes according to source width. `peak_live_bytes` for full runs was
329, 331, 787 and 592 MiB in table order. These are host diagnostic values,
not process RSS. Scratch and retained-owner peaks are not exposed separately
by this benchmark (`N/A`), so no separate peak claim is made.

The Float32→UInt8 optimization was guided by Instruments CPU Profiler. The
pre-SIMD trace (`build/fmt06-f32u8-before.trace`, 3 executions) has 6,958 sampled
stacks including setup, with leaf counts of 2,118 `_platform_memmove`, 1,383
`convert_one`, 1,353 `Value::element_size` and 854 memcpy stubs. These observations
motivated contiguous span kernels and hoisting element widths out of the loop.

The final NEON trace used a longer repeated execution window:

```sh
xcrun xctrace record --template 'CPU Profiler' --output build/fmt06-f32u8-after.trace --launch -- "$PWD/build/examples/numeric_conversion_performance/photospider_numeric_conversion_performance" 4096 f32-u8 full 20
xcrun xctrace export --input build/fmt06-f32u8-after.trace --xpath '/trace-toc/run[@number="1"]/data/table[@schema="cpu-profile"]' --output build/fmt06-f32u8-after.xml
```

Of 4,199 sampled stacks including setup, 2,511 contain the conversion planar
callback and 2,233 have `neon_f32_u8` as the leaf. Repeated element-width lookups
and sample-wise `convert_one` calls are no longer the dominant conversion
work. The recordings have different repetition counts and include setup;
leaf counts are diagnostic evidence, not comparable durations or speedup ratios.
The table reports separate, uninstrumented execution measurements.

## amd64 AVX2 measurement on 2026-09-24

Ubuntu WSL2 on Intel Core i9-12900 (24 logical CPUs, AVX2), Clang 18.1.3,
RelWithDebInfo, one worker, 4 KiB host pages. Remote source/build location:
`/home/alex/photospider-fmt06-20260924`, accessed through `ssh winpc` and
`wsl.exe -d Ubuntu`. Workload, publication boundary, disabled result cache and
three-execution timing policy match the Apple measurements above. Commands
ran serially without a concurrent build.

| Pair | Scalar dispatch full repeat | AVX2 full first | AVX2 full repeat | Speedup | AVX2 channel repeat | AVX2 3×3 repeat |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| UInt8→Float32 | 654.80 ms | 92.38 ms | 43.32 ms | 15.1× | 10.31 ms | 0.095 ms |
| Float32→UInt8 | 1030.74 ms | 47.61 ms | 37.87 ms | 27.2× | 11.06 ms | 0.149 ms |
| Float64→Float32 | 615.15 ms | 154.29 ms | 77.88 ms | 7.9× | 19.68 ms | 0.094 ms |

The reference changes only the local benchmark checkout's `use_avx2` dispatch
to `false`, then rebuilds the same targets; it retains width hoisting and span
batching. After measuring the reference, runtime CPU detection was restored,
the targets rebuilt, and `test_numeric_conversion` passed again. No dispatch
override parameter or accelerated operation name is exposed in the product.
The functions use `target("avx2")`; the library does not require global AVX2
compiler flags. A physical CPU without AVX2 was not available; the scalar
fallback was exercised by disabling dispatch on the AVX2 host.

Int64→UInt8 uses the portable exact integer span kernel, with full first/repeat
79.67/63.86 ms, single-channel repeat 16.10 ms, and cross-tile repeat 0.118 ms.
Its timing is not attributed to AVX2. Full peak live diagnostic bytes for
UInt8→Float32, Float32→UInt8, Float64→Float32 and Int64→UInt8 are respectively
358,878,400; 365,956,288; 867,175,616; and 656,411,840. Logical source/output bytes
match the Apple workload; different host page sizes affect backing/metadata.
The 3×3 ROI still reads exactly 9, 36 or 72 source bytes.

`test_numeric_conversion` passed with normal AVX2 dispatch and forced scalar
fallback on this host, and with NEON on Apple M5. It covers all 256 UInt8 codes,
259-sample vector tails, exceptional lanes, Float32 subnormal narrowing,
4,096 randomized Float64 conversions against a hardware cast oracle on both
generic and planar paths, and 1,024 randomized Int64 mappings against independent
128-bit division/remainder rounding. The direct floating-environment test covers
the generic callback; direct planar FTZ/DAZ environment restoration is not
separately tested. Independent code review found no required corrections to
the NEON/AVX2 kernels or dispatch.

## Optional SME experiment

The separately requested experiment follows commit `5fa9b10c`. Its standalone
target is disabled by default and is not linked into the kernel. The optimized
Float32 tile candidate subsequently moved into its own production TU, described
below. Build and run on an Apple arm64
host with SME and SME_F64F64:

```sh
cmake -S . -B build -DPHOTOSPIDER_BUILD_NUMERIC_CONVERSION_SME_EXPERIMENT=ON
cmake --build build --target photospider_numeric_conversion_sme_experiment -j 8
build/examples/numeric_conversion_performance/photospider_numeric_conversion_sme_experiment 4194563 7 v1
build/examples/numeric_conversion_performance/photospider_numeric_conversion_sme_experiment 67108864 5 v1
```

Only `sme_kernels.cpp` is compiled with `-march=armv8-a+sme+sme-f64f64`.
The ordinary host driver checks both runtime features before invoking the
locally streaming functions. M5 reports a 64-byte streaming vector length.
The candidates use predicated streaming SVE for elementwise conversion; they
do not use ZA matrix outer products. Disassembly confirms SMSTART/SMSTOP and
SVE conversion/load/store instructions at the expected function boundaries.

This is a span-kernel experiment, not public workflow latency. Inputs and output
buffers are allocated and touched before timing. One warmup pair is excluded;
NEON and SME order alternates each round, and the reported value is the median
of the subsequent paired rounds. The NEON functions mirror the committed
kernels. The benchmark compares 64-sample spans (the production work-check
boundary), 128-sample spans and 4096-sample spans (amortized mode switching).
No budget/cancellation or publication cost is included. UInt8 and Float32 inputs
cycle all 256 codes; Float64 uses nonzero normal values `(code+1)/256`.
Exceptional scalar fallback copies precomputed reference bytes and is not a
measurement of production fallback arithmetic.

Every timed output is compared byte-for-byte with an independent scalar
reference. Additional checks cover 4099 random Float32/Float64 samples,
predicated tails, NaN, out-of-domain values, signed zero, subnormal output and
overflow admission, with canaries confirming that a rejected vector writes no
suffix. These checks qualify the experiment's admitted domains; they do not
establish a complete production SME backend contract.

On the same Apple M5 and Clang 21.1.3 host, 4,194,563 samples, seven measured
rounds, 64-sample spans:

| Pair | NEON median | SME median | SME / NEON latency |
| --- | ---: | ---: | ---: |
| UInt8→Float32 | 0.352 ms | 0.778 ms | 2.2× |
| Float32→UInt8 | 1.780 ms | 18.984 ms | 10.7× |
| Float64→Float32 | 1.700 ms | 32.510 ms | 19.1× |

For 4096-sample spans the respective NEON/SME times were 0.261/0.554,
1.701/18.058 and 1.700/31.864 ms. All kernels accepted their entire valid input;
the slowdown is not an accidentally repeated scalar fallback. Its detailed
microarchitectural cause has not been established.

The isolated 67,108,864-sample run (five measured paired rounds) confirms the
result at the full-image sample count:

| Pair | 64-span NEON / SME | 128-span NEON / SME | 4096-span NEON / SME |
| --- | ---: | ---: | ---: |
| UInt8→Float32 | 4.134 / 10.795 ms | 4.191 / 8.428 ms | 4.168 / 8.285 ms |
| Float32→UInt8 | 28.382 / 301.932 ms | 31.364 / 322.690 ms | 28.011 / 289.181 ms |
| Float64→Float32 | 28.304 / 524.348 ms | 27.824 / 511.437 ms | 27.825 / 502.782 ms |

The randomized and exceptional-lane checks passed in both measurement runs.
ClangFormat 21, cpplint and diff checks passed. Independent read-only review
found no blocker or required finding in the experiment.

Initial decision: the three v1 candidates showed no benefit and were not
promoted. The follow-up below changes this decision for eligible Float32→UInt8
tiles after optimizing the data flow and measuring public execution.


## Deep SME optimization and whole-tile execution

The experiment now accepts a fourth `variant` argument (default `throughput`)
and compares 64, 128, 4096, 16384 and 65536 sample blocks:

| Variant | Controlled implementation change |
| --- | --- |
| `v1` | Original per-vector predicate checks and immediate scalar branches. |
| `batched` | Combine predicate checks across each 64-sample block. |
| `flags` | Accumulate integer flags; generate a predicate only at block end. |
| `host` | NEON admission per 64 samples followed by unchecked SME conversion. |
| `scheduled` | Eight independent double vectors, computed before the block decision. |
| `span` | One integer admission decision for the entire span; UInt8 uses a split-reciprocal FMA candidate. |
| `throughput` | Whole-span admission, four independent accumulators, all-true predicates for full vectors, and unrolled conversion. |
| `bits` | Exact integer-only conversion after the same whole-span admission. |
| `host-span` | NEON admission for the whole span before entering SME. |
| `unrolled` | Four independent integer-only Float32 conversion chains. |

The original vector-to-predicate-to-branch dependency hypothesis is consistent
with the measured improvement from changing this data flow. Predicate selection
and accumulator chains also mattered; the variants change more than one
instruction in some transitions, so timings do not isolate a universal cost
per branch or establish one sole microarchitectural cause. The cited
[Streaming SVE compiler study, section 3.3](https://arxiv.org/html/2506.02233v1#S3.SS3)
provides the relevant synchronization model; its measurements are not M5
measurements for this operator.

At 4,194,563 samples and five paired measured rounds, representative
Float32→UInt8 results progressed from approximately 18.3 ms (`v1`, 4096 block)
to 9.1 ms (`batched`), 3.5 ms (`flags`), and 1.1 ms (`throughput`). The
`throughput` 128×128 and 256×256 blocks measured 1.302 and 1.003 ms versus
NEON 1.789 and 1.793 ms. These isolated kernels omit host polling and publication.
UInt8's division/FMA variants remained slower than NEON. Float64 did not show
sufficient repeatable benefit for production selection.

The production implementation therefore selects SME only for the already
specialized Float32 `[0,1]` → UInt8 `[0,255]` map. A fully requested contiguous
rectangle containing 4,096–65,536 samples is processed in one streaming scope.
This includes complete 128×128 and 256×256 tiles; partial-width or padded
rectangles retain the row implementation. Numerical admission finishes before
any output store. Rejected domains use the exact scalar/NEON fallback.

The host admits work before the SME call. Failed admission and subsequent
fallback both consume work. An internal, allocation-free borrowed view retains
no extra owners and reads the existing monotonic cancellation flags with acquire
ordering. Admission polls every 64 samples, conversion every 32, and tails more
frequently. The source token remains alive throughout the synchronous call.
The host performs its final cancellation/currentness check before publication.
There is one SMSTART/SMSTOP pair per call, with scalar LDAPRB polling inside the
streaming scope; no repeated mode transitions are needed for cancellation.
Runtime admission requires SME, SME_F64F64 and a 64-byte streaming vector.
Compiler admission checks the ACLE header and locally-streaming function syntax.

The first successful whole-tile CPU Profiler recording is
`build/fmt06-sme-tile.trace` (20 public executions). Of 3,226 sampled stacks,
1,537 contain `sme_f32_u8_tile`; this confirms actual dispatch in the public
workflow. Setup is included and these counts are not elapsed-time shares.
The trace predates the last scalar polling-pointer cleanup and the 256 tile run.

Focused verification includes both 128 and 256 tiles, random Float32 samples,
all quantization thresholds and neighboring values, a NaN at the last sample,
clip fallback, an ROI excluding that NaN, deterministic pre-cancellation with
multiple flags, resource tests, and installed consumption. The optional command
`build/test_numeric_conversion_sme --midflight` observed cancellation after a
nonempty output prefix; output is inspected only after joining the worker.
That timing-dependent diagnostic is excluded from the deterministic CTest and
returns 2 if scheduling makes the observation inconclusive. Independent review
found and closed the initial cancellation-cadence regression.

Reproduce production selection and compare against the same source with SME off:

```sh
cmake -S . -B build -DPHOTOSPIDER_ENABLE_NUMERIC_CONVERSION_SME=ON
cmake --build build --target photospider_numeric_conversion_performance test_numeric_conversion test_numeric_conversion_sme -j 8
ctest --test-dir build -R '^(test_numeric_conversion|test_numeric_conversion_sme)$' --output-on-failure
build/examples/numeric_conversion_performance/photospider_numeric_conversion_performance 4096 f32-u8 full 7 128
build/examples/numeric_conversion_performance/photospider_numeric_conversion_performance 4096 f32-u8 full 7 256
# Repeat after configuring PHOTOSPIDER_ENABLE_NUMERIC_CONVERSION_SME=OFF.
```

Production A/B results on Apple M5, one worker, cache off, 4096²×4 Float32
input: each process runs seven executions, excludes the first, and reports
the mean of six repeats. The full-image figure below is the median of three
process means; all timing commands ran serially after their build completed.

| Tile size | NEON full | SME full | Time reduction | NEON / SME one channel |
| --- | ---: | ---: | ---: | ---: |
| 128×128 | 34.904 ms | 30.640 ms | 12.2% | 8.822 / 7.236 ms |
| 256×256 | 34.493 ms | 28.637 ms | 17.0% | 8.249 / 6.938 ms |

The three full process means were NEON 36.837/34.904/34.735 ms and SME
31.011/30.177/30.640 ms for 128 tiles; NEON 37.917/34.493/33.911 ms and SME
28.896/28.637/28.057 ms for 256 tiles. First-execution latency ranged roughly
37–54 ms. The small cross-tile ROI remained on the row path (about 0.03 ms;
individual observations varied up to 0.04 ms). The SME gate adds no full-image
scratch: full output backing remained 67,108,864 bytes and peak live diagnostic
bytes 347,081,920. Diagnostic source demand remained 268,435,456 bytes, and
36 bytes for the 3×3 ROI; SME admission makes an additional in-memory pass over
already demanded source data, so these counters are not physical load traffic.

No additional accelerated or SME operation name is registered. The production
option defaults on for Apple arm64 when the compiler probe succeeds; runtime
capability mismatch falls back to NEON. The standalone experiment remains an
explicit build option.
