# Perlin 2002 strict Result execution

The registry exposes three Result operations: noise.perlin2002_3d_v1_strict_cpu_whole, noise.perlin2002_3d_v1_strict_cpu_tiled, and noise.perlin2002_3d_v1_strict_gpu. Each accepts one Result containing one numeric tensor coordinates[S...,3] and publishes the generic numeric tensor samples[S...] on the values output port. The implementation preserves the fixed Perlin 2002 permutation and exact integer polynomial, with one final IEEE-754 rounding.

## Result interface and demand

The input Result has one tensor member with any valid member key; coordinates names its semantic role in this operation. The tensor has Float32 or Float64 elements, rank 2..8, positive extents, and a final extent of three. The complete coordinate tensor contains at most 2^40 elements. Valid numeric facets are accepted on input; they do not change how coordinate bits are interpreted. The output uses schema photospider.tensor version 1, whose tensor key is samples and whose sample shape is S...; the operation output port is values. It carries no inferred color semantics. The optional static String parameter dtype selects float32 or float64; its default is float64. Upstream operations define coordinate construction, frequency, and units.

Whole and GPU declare Whole demand and dirty behavior: execution reads and validates all coordinates, and a change to any input sample dirties the full output. CPU tiled declares a Result Dependency-v2 map. For each requested output sample it requests the corresponding three coordinate components as Data and Validation support. Its output descriptor is declared separately. Empty output requests perform no input payload read and no Perlin arithmetic.

Tiled execution partitions each requested box into bounded output tiles. Leading sample axes advance one sample at a time; the last two sample axes use the requested tile height and width. The continuation publishes each completed tile in traversal order, retaining a valid Result prefix while more tiles remain. CPU tile callbacks run through the host cpu_tiles service for the current granted stage. This execution does not schedule a concurrent window of output tiles. The independent Whole path uses the host range-parallel service when available.

The tiled kernel first decodes every coordinate in the current tile, then evaluates those decoded values. Its decoded record occupies 72 bytes per output sample. The current declared input workspace allowance is six times the coordinate payload bytes for that tile; output and continuation storage are admitted separately. Work, scratch, output, continuation, and Result publication metadata remain subject to the execution root limits. Empty requests skip these allocations.

## Exact numerical contract

For each coordinate, binary input decoding obtains exact floor, modulo 256, and dyadic fractional values. The fade polynomial is

$$
f(t)=6t^5-15t^4+10t^3.
$$

The fixed 256-entry permutation and grad(hash & 15) rule select the eight corner gradients. The CPU computes the exact weighted sum with seven nested lerps; the native shader uses the expanded eight-corner polynomial. The permutation and gradient definition are specified in [NOI_perlin2002_permutation](op_specs/NOI_perlin2002_permutation.md). Both forms preserve the exact result and round only once to the requested output format, using ties-to-even. Integer lattice points return positive zero. A negative nonzero result that underflows to zero retains negative zero. Floating-point intermediate arithmetic is not used. The periodicity is 256 on each axis, the conservative magnitude bound is 2, and the contract does not claim strict [-1,1] range or Java bit identity.

With common denominator D=2^q, the conservative numerator bound needs at most 16q+5 bits. CPU arithmetic uses 8, 16, or 272 64-bit limbs for q<=31, q<=63, or q<=1074. GPU scratch uses 17 little-endian base-2^32 integers per active lane and selects 16, 32, or 544 words at the same tier boundaries. Float64 values travel as bits, so device Float64 arithmetic support is not required.

Each CPU sample reserves a conservative arithmetic work bound before its multiplications and checks the local credit during evaluation. At q=1074, PerlinExact::work_bound is 763,702 units per sample. Coordinate decode and addressing are charged before the reads. GPU admission scans input bits to select the limb tier and bound device work; shaders compute all output values. Work is a modeled operation count, not a device instruction count.

## Native GPU memory and execution

The GPU registration requires the native GPU service and has no CPU fallback. It submits the MSL/Metal or SPIR-V/Vulkan shader selected by that service. Each dispatch writes a disjoint output range and reuses scratch only after synchronous completion. Fast tiers use batches of at most 256 samples and group up to eight dispatches per synchronous submission. The full precision tier uses at most four samples per dispatch and one dispatch per submission. The largest scratch allocation is 557,056 bytes in the q<=63 tier; the q>63 full precision tier uses at most 147,968 bytes.

An authorized affine Result read window keeps its backing owner, byte offset, signed strides, and logical origin alive through shader execution. A same-device native owner can be bound directly without a transfer. When the authorized window is not one physical affine view, the host materializes the requested window into a dense native input buffer and accounts for that transfer. The shader address calculation supports negative and broadcast strides, padding, nonzero origins, and unaligned coordinates. Result owners retain published output after the execution context retires.

Metal has a 232-byte argument block. Vulkan packs a 432-byte std140 argument block and requires 64-bit integer arithmetic and 8-bit storage-buffer support. The focused Result GPU test passes on native Metal, including same-device affine input and retained output after context retirement. Installed-consumer checks pass for the test and example targets. The independent Fraction oracle passes 1,566 cases for CPU Whole, CPU tiled, and Metal GPU. Earlier Vulkan execution evidence belongs to the former Value path and does not validate this Result path.

## Failures and limits

Nonfinite coordinates fail validation. Invalid shape or dtype metadata fails specialization. Arithmetic work, allocation, stage, or cancellation failures do not publish a successful final result; tiled execution may already have published earlier tiles as a Result prefix. Cancellation stops later GPU submissions, while submitted synchronous work retires before its owners are released. A missing GPU service or unsupported backend returns a backend error without CPU fallback.

## Reproduction

The public [workflow example](../../../examples/perlin_workflow/README.md) uses Result bindings and the values output port. Current focused validation passes four Perlin tests, six installed-consumer tests, and the independent Fraction oracle's 1,566 cases for CPU Whole, CPU tiled, and Metal GPU. The GPU checks include same-device affine input with zero transfer, two-chunk input with one transfer of 48 bytes, retained output after context retirement, cancellation release, and CPU bitwise comparison. The example sets the dependency stage limit to 3*N+1 for its requested sample count N.

    cmake --build build/kernel-dev --target test_perlin_exact test_perlin_workflow test_perlin_tiled test_perlin_gpu -j 8
    ctest --test-dir build/kernel-dev -R '^test_perlin_(exact|workflow|tiled|gpu)$' --output-on-failure
    python3 oracle/ops/generation/check_perlin_runtime.py --runner build/kernel-dev/test_perlin_workflow
    python3 oracle/ops/generation/check_perlin_runtime.py --runner build/kernel-dev/test_perlin_workflow --tiled
    python3 oracle/ops/generation/check_perlin_runtime.py --runner build/kernel-dev/test_perlin_gpu
    cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
    cmake -S tests/consumer -B build/kernel-dev/consumer-build -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build/kernel-dev/consumer-build --target photospider_perlin_workflow_consumer photospider_perlin_tiled_consumer photospider_perlin_gpu_consumer photospider_perlin_workflow -j 8
    ctest --test-dir build/kernel-dev/consumer-build -R '^installed_perlin_' --output-on-failure

The tiled fixture requests y=[3,14), x=[5,32) from a 17-by-35 coordinate field with tile height 4 and width 8. It checks 12 bounded source reads totaling 7,128 bytes, 12 ordered tile deliveries covering 297 samples, and ignores a NaN outside the requested region. A separate Float32 multi-box request covers 15 samples and checks finite-work, scratch-capacity, and in-region NaN errors. The independent Fraction oracle checks a finite fixture set, not the full mathematical domain or all GPU devices. Earlier benchmark sections below recorded the former Value execution path. They remain historical measurements and do not characterize current Result scheduling, transfer costs, work totals, or latency.

## Historical benchmark records from the former Value execution path

These measurements and test observations are retained for their original builds. They do not characterize current Result scheduling, work totals, transfers, or latency. The former tiled workflow used concurrent output windows; current CPU tiled execution submits one granted tile stage through cpu_tiles.

## Measured exact arithmetic changes

The former Value execution path paired Apple M5/macOS 27.2 benchmark compares the original CPU eight-term weighted polynomial with the exact seven-lerp form. It uses 16,384 Float64 output samples and reports the median of five per-process measured-call medians; ranges below are the minimum and maximum of those medians. Every paired row reports bitwise equality with the Whole reference.

| Mode | Workers | Weighted polynomial, ms | Nested lerps, ms | Paired observation |
| --- | ---: | ---: | ---: | --- |
| Whole | 1 | 23.607 (22.860–24.714) | 13.575 (13.249–13.964) | Declared arithmetic work falls from 63,045,632 to 29,540,352. |
| Whole | 4 | 8.360 (8.143–8.994) | 5.927 (5.704–6.312) | Output and workspace are unchanged. |
| Tiled | 4 | 18.446 (14.206–19.547) | 15.881 (14.900–16.641) | Improvement is smaller; the timing ranges overlap. |

These are results from this fixed workload and benchmark host only. They do not establish relative performance on other processors or GPUs. Raw rows are in `out/performance-review/perlin-lerp-paired.json`.

## Measured budget-admission optimization

The budget and tiled timing values in this section describe their recorded benchmark builds. The tiled issued-work figures predate the bounded scalar-read accounting described above and are not current-code work totals.

`test_perlin_workflow --benchmark` constructs deterministic Float64 triples from `((i*1709+719)%131071)/65536-1`, with 16384 output samples. It performs one warmup and five measured calls for each of 1/4/8 host workers, checks output bytes against the one-worker result, and reports execute-only latency. Registry/context creation, compilation, input construction, root-statistics collection and result comparison are outside the timer. No profiler or build overlaps these timing runs.

On Apple M5/macOS 27.2, the initial per-row global budget updates measured 27.0622/226.403/432.474 ms median for 1/4/8 workers. A separate native `sample` trace locates the contention in `ResourceBudget::try_consume` called from the inner multiply rows. Per-sample precharge measures 17.5412/5.03512/4.18125 ms, with identical output bytes. The root's issued work rises from 62,844,998 to 63,045,632 because the bound includes unused conservative credit. Peak modeled host reservation is 3,196,560 bytes for each worker count. This case demonstrates an actual accounting-contention fix; it does not cover all denominator tiers, request sizes or sustained mixed-workload throughput.

Raw timings and the separate profiler trace are under `out/gpu-whole-tiled/raw/perlin-whole-macos-{initial,precharge}.jsonl` and `perlin-whole-initial.sample.txt`. Sample counts include blocked stacks and are not CPU percentages. That recorded build used a maximum prepaid bound of 3,432,111 units per sample at q=1074; the current Result source uses 763,702.

On native FreeBSD 15.1/i9-12900, pinned to the same logical CPU set `0,2,4,6,8,10,12,14`, initial 1/4/8-worker medians are 47.4365/63.9797/70.0652 ms. The precharge implementation measures 39.3109/10.1356/5.19177 ms. Output bytes, declared work and modeled peak match the corresponding macOS workload. Timing files are `out/gpu-whole-tiled/raw/perlin-whole-freebsd-{initial,precharge}.jsonl`. The historical fully instrumented macOS TSAN run passed the arithmetic and public workflow tests without a race report; raw test logs are in `raw/perlin-tsan-tests.txt`.

## Native arithmetic-only paired measurements

A separate FreeBSD 15.1 amd64 harness run used Clang 22 and pinned execution to CPU 0. It ran three fixed-order rounds rather than alternating before and after. The same isolated arithmetic harness linked against a compatible existing kernel static library. Matching checksums provide a consistency check, but the timing harness did not compare all output bytes. Separate oracle and unit tests establish arithmetic correctness. These timings exclude public workflow setup, scheduling and publication.

| Arithmetic case | Samples | Earlier headers, ms | Current headers, ms |
| --- | ---: | ---: | ---: |
| Perlin full precision, q=63 | 1,000 | 16.271 (16.253–16.297) | 15.453 (15.420–15.546) |
| Perlin full precision, q=1074 | 100 | 15.276 (15.247–15.660) | 4.985 (4.969–5.010) |

These three-round arithmetic measurements are limited evidence for the exact kernel expressions. They do not describe a full workflow or establish a gain on other CPUs. The harness and raw measurements are in ignored `out/performance-review/`. The tracked arithmetic and workflow validation commands are listed under [Reproduction](#reproduction); they verify correctness and do not reproduce this timing harness.

## Measured tiled scheduling and traversal

The former Value public example timed execution and copying the complete result into the same client-owned byte array for both modes. Setup, compilation and comparison with a separately executed one-worker Whole reference are outside the timer. Each trial uses one warmup and five measured calls. Tile size 128 at 16384 samples produces 128 real computation callbacks.

| Platform | Workers | Per-sample traversal/charging, ms | Tile predecode/precharge, ms |
| --- | ---: | ---: | ---: |
| M5/macOS 27.2 | 1 | 29.3731 | 27.5539 |
| M5/macOS 27.2 | 4 | 19.2420 | 10.4321 |
| M5/macOS 27.2 | 8 | 28.7132 | 10.6401 |
| i9-12900/FreeBSD 15.1, pinned as above | 1 | 84.2646 | 47.6296 |
| i9-12900/FreeBSD 15.1, pinned as above | 4 | 18.6298 | 14.6644 |
| i9-12900/FreeBSD 15.1, pinned as above | 8 | 19.8955 | 9.76525 |

All outputs match Whole bytes. Issued work remains 63,084,919. On macOS, modeled peak host reservation for 1/4/8 workers is 481672/701096/928128 bytes after predecode, versus 481672/626976/869608 before it. A separate native sample trace contains shared `DependencySession::Impl::consume` locking and per-point visitor allocation in the original hot path; blocked-stack counts are not CPU shares. For a single 128-sample tile, measured 8-worker latency changes from 0.240125 to 0.256375 ms, so the large-input gains do not establish a small-request gain.

In a separate earlier M5 measurement, tile widths 256/512/1024 at 16384 samples gave 4-worker medians 8.36079/7.28896/6.77254 ms and 8-worker medians 6.63575/5.53517/4.61108 ms. The 8-worker modeled peaks grow to 935656/1205456/1785552 bytes. This is a latency/capacity tradeoff, not evidence that the largest tile is optimal for all workloads. The example keeps tile width explicit. Raw data are `out/gpu-whole-tiled/raw/perlin-tiled-*.jsonl`; the separate trace is `perlin-tiled-initial.sample.txt`.
