# Single-thread CPU FP32 validation and performance

Measured on 2026-09-25, Apple M5 (10 CPU cores, 32 GiB), macOS 27.2.
Slang 2026.18.2; plugin Release with Apple Clang 21.0.0; installed kernel 0.24.0,
RelWithDebInfo with Homebrew Clang 21.1.3. All computation translation units use
`-fno-fast-math -frounding-math -ffp-contract=off`; Slang uses precise FP.

The plugin runs serially on the kernel-assigned callback thread. The earlier
GCD/std::thread dispatcher and worker-count control have been removed. The kernel
owns scheduling. NEON or AVX2 processes independent pixel lanes within that one
thread. `PIXELOE_CPU_SIMD=0` selects generated Slang; the default admits SIMD
when supported. No new image-sized scratch buffers are needed by these paths.

## Controlled public workflow comparison

Both columns use the same current binary and one host workflow worker, one
untimed warmup, derived-result caching disabled. Each scalar run precedes its
SIMD counterpart. No local builds, oracle runs or profilers overlap these timings.
Default parameters: pixel_size=6, thickness=3, contrast downscale, color matching,
exact-table lowrank rank=1, no quantization/sharpening, post-upscale.

Compilation, module loading, graph compilation, codec/file I/O and input planar
construction are outside the timer. Execution allocations, planar-to-scratch
transfer, arithmetic and transactional output publication are inside it.

| Input W×H | Output W×H | Runs | Slang median ms | NEON median ms | Speedup | NEON p95 ms | Peak managed MiB |
|---|---|---:|---:|---:|---:|---:|---:|
| 128×128 | 132×132 | 9 | 10.11 | 6.24 | 1.62× | 6.46 | 2.02 |
| 1920×1080 | 1920×1080 | 5 | 1059.88 | 525.73 | 2.02× | 546.37 | 191.78 |
| 4096×4096 | 4098×4098 | 3 | 9146.83 | 4763.00 | 1.92× | 4794.36 | 1553.05 |

The synthetic RGB channels are deterministic FP32:
`((17*x + 31*y + 71*c + (x*y)%113)%256)/255`. Peak managed bytes are identical
between scalar and SIMD for every workload. With three repetitions p95 is the
maximum sample. These local samples are not a cross-platform latency guarantee.

## Real images, codec time excluded

| Source | Input W×H | Output W×H | Slang median ms | NEON median ms | Speedup | NEON p95 ms |
|---|---|---|---:|---:|---:|---:|
| pattern_rgba16_129x131.png | 129×131 | 132×132 | 9.28 | 5.83 | 1.59× | 5.83 |
| illustration_rgb8_4962x5454.jpeg | 4962×5454 | 4962×5454 | 14129.20 | 6962.93 | 2.03× | 7106.99 |
| stage_rgba8_3504x4958.tiff | 3504×4958 | 3504×4962 | 8909.19 | 4347.86 | 2.05× | 4376.36 |

Each source is from assets/codec and uses its full dimensions, one warmup and
three measured runs. Previously decoded FP32 fixtures are reused for both paths.
Preparation uses OpenCV, retains uint16 precision for the pattern PNG, reorders
BGR to RGB and explicitly discards alpha. Normalization, raw file reading and
planar construction finish before timing. No implicit ICC transform is performed.

## Hotspots and changes

The initial single-thread native probe measured 1139.25 ms at 1080p. Time Profiler
attributed the dominant work to lowrank blur, morphology, powf and median selection.
That initial trace overlapped another measurement and is used only for attribution.
The controlled workflow comparison above is the authoritative latency result.

- Lowrank row/column blur: SIMD across adjacent x coordinates, four independent
  accumulators, ascending tap order, separate multiplication/addition. Vertical
  offsets are reused per row. Boundary and tail pixels retain scalar equations.
- Morphology and outline blend: SIMD across adjacent pixels, retaining window
  order, zero padding, clamp, normalization and min/max zero semantics.
- All other stages retain the pinned AOT Slang implementation. CPU/OS admission
  guards AVX2, and only cpu_simd.cpp is compiled with AVX2 flags. Unsupported
  targets use Slang. Cancellation is checked per row in the specializations.

Exploratory native stage means before/after specialization, in milliseconds:

| Stage | Slang | NEON |
|---|---:|---:|
| Four morphology passes | 185.91 | 29.74 |
| Lowrank radius-32 row pass | 151.93 | 21.17 |
| Lowrank radius-32 column pass | 166.48 | 26.77 |
| Outline blend | 73.75 | 15.07 |

These probes are separate from the controlled public runs; they locate the
improvement and must not be added to or subtracted from public latency. The
post-change native probe measured 515.43 ms pipeline and 512.60 ms dispatch median.

The post-change Time Profiler trace contains 7061 samples, all on the native
benchmark's main thread. powf and its stub account for approximately 30% of leaf
samples; lattice statistics plus contrast median selection account for 22%.
The remaining Lab conversion/statistics and order selection are the next major
costs. Approximate vector transcendental functions and reassociated reductions
were not introduced, because they would require a different numerical contract.

## Numerical and platform validation

- Apple M5 NEON: **3720/3720 bitwise stage comparisons** against actual generated
  Slang kernels. Includes 12 widths, 5 heights, every lowrank radius and mode,
  six structuring-element tables, padding, vector tails, signed zero/subnormals,
  cancellation and service calls confined to the callback thread.
- Intel i9-12900 AVX2, FreeBSD, Clang 22.1.7: **3720/3720** same stage comparisons.
  Runtime admission was explicitly checked; no-ISA hosts return test exit 77.
  This is a standalone stage harness, not a FreeBSD installed-plugin workflow
  qualification. Slang's prelude lacks a FreeBSD platform branch; the harness
  uses its documented SLANG_PLATFORM override with SLANG_LINUX for POSIX macros.
- **40/40** independent upstream Torch/convolution/sliding-statistics cases pass;
  maximum absolute output error 3.2186508178710938e-6, bound 1e-5 per case.
- All 40 full-workflow cases match scalar/SIMD output bytes exactly; both paths
  independently pass the Torch oracle. Caller-rounding checks pass. The 1080p result also
  matches the pre-change single-thread Slang output byte for byte.
- Invalid input/parameters/budget, zero variance, partial Whole rejection,
  signed-zero/subnormal copies and RGB metadata admission checks pass.
- ClangFormat 21, cpplint and diff whitespace checks pass. Independent scoped
  review found no remaining blocker or required findings.

The full installed plugin/workflow and all performance numbers here were tested
on Apple M5. AVX2 stage validation does not establish Windows/Linux module behavior
or x86 workflow performance. This is not an exhaustive option Cartesian product,
a correctly-rounded transcendental implementation or a cross-libm bitwise claim.
The earlier eight-worker figures are superseded by this single-thread report.
The user's RTX 4090 CUDA/Vulkan figures were not reproduced in this CPU work.

## Raw results and reproduction

- build/pixeloe-single/performance.json and performance.log: controlled public runs.
- build/pixeloe-single/neon.log and avx2-final.log: ISA stage validation.
- build/pixeloe-single-validation/validation.json,
  build/pixeloe-single-scalar-validation/validation.json,
  build/pixeloe-single/workflow-bitwise.log and build/pixeloe-single/contract.log.
- build/pixeloe-single-before.trace and build/pixeloe-single-after.trace, plus their
  exported time-profile XML files: sampled attribution, not latency benchmarks.

See [README.md](README.md) for build, workflow, SIMD selection and test commands.
All generated data remains under ignored build/.
