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
`i64-u8`), `coverage` (`full`, `channel`, `tile`), and repetitions. `channel`
requests one complete plane. `tile` requests `[127,130) × [127,130)` on
channel 1, crossing both tile boundaries. For `u8-f32`, code 0 maps to 0,
128 maps to correctly rounded binary32 `128/255`, and 255 maps to 1. The
unrequested channels have no conversion demand.

## Measurement on 2026-09-24

MacBook Pro, Apple M5, 32 GiB RAM, macOS 27.2, Clang 21.1.3,
RelWithDebInfo, CPU strict profile, one worker, tiled input. Inputs are fully
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
exact rational evaluation. Work/cancellation checks remain at 64-sample bounds.

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
