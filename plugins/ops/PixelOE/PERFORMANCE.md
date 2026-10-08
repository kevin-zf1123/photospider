# PixelOE performance evidence

The measured CPU Whole, tiled, Metal and Vulkan results below come from the former planar integration path; they are historical and do not establish current Result workflow performance. The current public workflow uses Result operation ABI 2 and Whole Result images. Its `peak_bytes` value is the cumulative execution Root Host peak, not the earlier local managed/scratch ledger; `live_payload` is the positive delta over the bound source Result payload baseline. `work_budget` limits work during execution, while source fixture construction is outside that work limit. Current contract checks pass on CPU, CPU tiled and Metal. Across 126 cases (42 per mode), final CPU, CPU-tiled and Metal outputs match the corresponding earlier CPU/Metal outputs that passed upstream-oracle validation. CPU-tiled coverage is byte comparison, not a separate Torch oracle run. Independent upstream-oracle checks pass 42/42 for CPU and Metal, with maximum absolute errors 3.2186508178710938e-6 and 3.159046173095703e-6. The current Result Vulkan path has not been run. These correctness results provide no speed comparison for the Result path.

The historical CPU Whole plugin used host-assigned ranges and retained the NEON/AVX2
specializations. A host quota of one is the serial reference configuration.
Each stage joins before dependent stages begin. Fixed workgroup reduction order,
stable ordering, palette iterations and ordered dithering are independent of
worker count. The plugin created no threads. The tables below record CPU Whole behavior for that earlier path. CPU tiled and GPU paths have separate correctness and performance evidence; their results do not belong in these CPU comparisons.

## Controlled public workflow measurements

All runs use the public installed SDK workflow, one warmup and three measured
executions, derived-result caching disabled, SIMD enabled and default parameters:
pixel_size=6, thickness=3, contrast downscale, color matching, exact-table lowrank
rank=1, no quantization/sharpening, post-upscale. The input is deterministic FP32
RGB: `((17*x + 31*y + 71*c + (x*y)%113)%256)/255`.

Compilation, module loading, graph compilation, fixture I/O and input planar
construction are outside the workflow timer. Execution allocations, input
transfer, arithmetic and transactional publication are inside. Child process
resource counters include warmup and I/O and must not be interpreted as
per-execution counters. With three samples, p95 is the maximum sample. No build
or profiler ran concurrently on the measured host.

### macOS

Apple M5, 10 CPU cores, 32 GiB, macOS 27.2; Apple Clang 21; kernel 0.28.0
RelWithDebInfo, plugin Release, Slang 2026.18.2. All computation uses precise FP,
`-fno-fast-math -frounding-math -ffp-contract=off`.

| Input | Supplied snapshot 1 worker ms | Current 1 worker ms | Current 4 workers ms | Peak managed bytes |
| --- | ---: | ---: | ---: | ---: |
| 128×128 | 6.01604 | 6.00346 | 3.86583 | 2,113,442 |
| 1920×1080 | 536.706 | 543.192 | 157.184 | 201,096,482 |
| 4096×4096 | 4754.35 | 4781.75 | 1396.85 | 1,628,488,514 |

Every result is byte-identical to the original supplied snapshot plugin. The
large workloads show approximately 3.4× speedup. The range queue only broadcasts
on final retirement, uses targeted helper wakeups, and bypasses the shared queue
for serial ranges. A separate earlier parallel sample measured 3.10613, 158.099
and 1413.89 ms for the same sizes. Thus the 128² sample regressed while large
samples changed little; these runs do not establish a robust gain from the
notification changes alone. Whole-child involuntary context switches changed
from 5272 to 4931 at 1080p and 55997 to 49432 at 4096².

### FreeBSD

FreeBSD 15.1 amd64, Intel i9-12900, 24 logical CPUs, 64 GiB, Clang 22.1.7;
native Release AVX2 plugin built against the installed 0.28.0 SDK. The portable
Slang source bundle is compiled natively; no Linux executable is used.
The complete benchmark and its children are restricted with
`cpuset -l 0,2,4,6,8,10,12,14`. CPUID leaf 0x1a on every selected logical CPU
reported EAX 0x40000001. Both worker configurations use this same set.

| Input | Current 1 worker ms | Current 8 workers ms | 8-worker p95 ms | Peak managed bytes |
| --- | ---: | ---: | ---: | ---: |
| 128×128 | 26.773 | 24.1758 | 27.1186 | 2,060,450 |
| 1920×1080 | 2897.96 | 471.149 | 476.274 | 202,473,122 |
| 4096×4096 | 23675.9 | 3770.57 | 3778.92 | 1,641,648,962 |

Every output is byte-identical across worker counts. The reference is the
current one-worker implementation, not the supplied snapshot. Unpinned trials
showed substantial one-worker variability (4096² 23676.9–45623.4 ms), so they are
retained separately and are not used for the speedup comparison. At 128², the
small gain indicates that fixed and scheduling costs require further work.
Hardware counters and stage-level FreeBSD profiling remain to be collected.
These samples are not cross-platform latency guarantees or architecture-normalized
comparisons.

## Numerical and concurrency evidence

- macOS NEON and native FreeBSD AVX2: each passes 3720/3720 stage comparisons
  against generated Slang, including widths, tails, padding, all lowrank radii,
  morphology tables, signed zero, subnormals and cancellation.
- macOS: 40/40 option profiles pass the independent upstream Torch/convolution/
  sliding-statistics reference; maximum absolute error 3.2186508178710938e-6,
  within 1e-5. All 40 outputs are byte-identical to the supplied snapshot.
- Native FreeBSD: all 40 profiles produce identical bytes with 1 and 8 workers.
  Actual FreeBSD output files were retrieved and checked locally against the
  independent Torch reference; all 40 pass, with the same maximum error.
- Fully instrumented macOS ThreadSanitizer: all 40 full-workflow profiles pass
  1-versus-4-worker byte comparison without a race report. Core range/planar
  tests also pass. This is not FreeBSD sanitizer evidence.
- Contract checks cover scalar/SIMD and worker equality, caller floating
  environment, invalid input/parameters, budget failures, zero variance,
  signed-zero/subnormal copies, RGB metadata and partial Whole rejection.

## SIMD blur tail paired measurements

After the low-rank blur's four-vector loop, the SIMD path processes any remaining
full vectors one at a time, then handles the final partial vector with scalar code.
Each vector lane retains ascending tap order, and row boundaries stay unchanged.
On Apple M5/macOS 27.2, five paired CPU Whole workflow runs measured these input
sizes with one worker. Each table value is the median of five per-process
execution medians; the range gives the minimum and maximum of those medians.

| Input | Before, ms | One-vector tail, ms | Observation |
| --- | ---: | ---: | --- |
| 24 × 256 | 3.381 (3.370–3.391) | 3.137 (3.120–3.181) | 7.2% lower median; checksums and peak bytes match. |
| 128 × 128 | 6.049 (5.917–6.073) | 5.886 (5.870–5.910) | 2.7% lower median; checksums and peak bytes match. |
| 256 × 256 | 19.694 (19.524–19.740) | 19.601 (19.543–19.846) | Ranges overlap; no stable gain established. |

The change passed 4,030 bitwise SIMD/AOT comparisons, CPU and tiled contract
checks, and the stage check. These measurements support a small and workload-
dependent CPU Whole improvement. They do not show a gain for every image size or
establish a broader PixelOE speedup. Raw paired rows are in
`out/performance-review/pixeloe-tail-paired.json`.

The maintained source builds and checks can be run with:

```sh
cmake --build build/pixeloe --target photospider_pixeloe pixeloe_workflow pixeloe_check_simd pixeloe_check_stages -j 8
build/pixeloe/pixeloe_check_simd
build/pixeloe/pixeloe_check_stages
build/pixeloe/pixeloe_workflow build/pixeloe/libphotospider_pixeloe.so 24 256 backend=cpu workers=1 warmup=1 repeat=5
```

The final command measures five current executions after one warmup using the
workflow's deterministic default RGB input. It reproduces a current-build sample,
not the paired comparison; the old-build measurements and exact paired runner
invocation are not part of the tracked source tree.

## CPU Whole quantization label buckets

The CPU Whole path with its SIMD-enabled dispatcher accumulates each chunk's
label buckets directly into that chunk's existing `int64[K*4]` partial region.
Each chunk visits samples in their original order, and `km_update` keeps its
prior sequence. The change reduces repeated pixel scans for full-grid K-means
while preserving the partial-table allocation and memory peak. It does
not change scalar CPU, CPU tiled, generated-kernel or GPU execution. The bucket
accumulation itself uses the existing scalar fixed-point label routine inside
the SIMD-enabled specialization; this is not vector arithmetic over labels.

Five alternating public-workflow pairs used one warmup and seven timed executions
per process. The high-cost case used a 256 × 256 RGB input, `pixel_size=2`,
`num_colors=128`, `do_quant=true`, `do_color_match=false`, `thickness=0` and
`no_post_upscale=true`.

| Input and worker count | Existing path, ms | Chunk buckets, ms | Observation |
| --- | ---: | ---: | --- |
| Heavy quantization, 1 worker | 79.242 (78.947–79.885) | 63.774 (63.288–64.210) | 19.5% lower median. |
| Heavy quantization, 4 workers | 22.365 (22.156–22.421) | 18.754 (18.669–24.350) | 16.2% lower median; the bucket arm includes one high sample. |
| Default quantization, 512 × 512, 1 worker | 79.724 (79.545–80.673) | 78.585 (78.377–79.055) | 1.4% lower median. |
| Default quantization, 512 × 512, 4 workers | 25.997 (25.699–30.844) | 25.353 (25.320–25.757) | Small change with broad baseline spread. |

Checksums and peak managed bytes matched for each pair; the high-cost case used
3,514,302 bytes in both arms. The default case shows only a small difference.
Repeat-k-means also changed only slightly, from 26.188 to 26.071 ms at four
workers. These results support the heavy full-grid K-means case, not a general
quantization speedup or a reduction in scratch memory. Raw data are in
`out/performance-review/pixeloe-buckets-heavy-paired.json` and
`pixeloe-buckets-paired.json`.

Build the maintained public workflow and run a current heavy-case sample with:

```sh
cmake --build build/pixeloe --target photospider_pixeloe pixeloe_workflow -j 8
build/pixeloe/pixeloe_workflow build/pixeloe/libphotospider_pixeloe.so 256 256 backend=cpu workers=1 warmup=1 repeat=7 do_quant=true pixel_size=2 num_colors=128 do_color_match=false thickness=0 no_post_upscale=true
build/pixeloe/pixeloe_workflow build/pixeloe/libphotospider_pixeloe.so 256 256 backend=cpu workers=4 warmup=1 repeat=7 do_quant=true pixel_size=2 num_colors=128 do_color_match=false thickness=0 no_post_upscale=true
```

These commands measure the current build using the workflow's deterministic
input. They do not recreate the alternating before/after experiment.

The finite option suite is not an exhaustive Cartesian product. Approximate
vector transcendental functions and reassociated reductions are not introduced.
No correctly-rounded transcendental or cross-libm bitwise claim is made.

## Artifacts and reproduction

Artifacts are under `out/gpu-whole-tiled/`:

- `pixeloe-whole-notification-tuned/whole.jsonl`: macOS controlled runs.
- `pixeloe-whole-initial/whole.jsonl`: separate initial parallel measurements.
- `freebsd-whole-pinned/whole.jsonl`, `freebsd-whole-unpinned/whole.jsonl`.
- `pixeloe-baseline-equivalence/validation.json`: actual baseline comparison.
- `pixeloe-tsan-workers/workers.json`: instrumented option comparisons.
- `freebsd-worker-results/`: actual native output files and comparison records.
- `raw/pixeloe-whole-before.sample.txt`: separate sampled stacks; blocked samples
  are not CPU percentages and profiling-run timing is not a controlled result.

Use `tools/benchmark_whole.py --baseline PATH --build PATH --out PATH --workers N`
for a byte-checked public comparison. When the reference is the current build,
set `--reference-label current-whole-one-worker`. See [README.md](README.md) for
installed SDK builds, portable source export and validation commands.


## Native GPU correctness and limited Vulkan performance

On FreeBSD 15.1 with Intel UHD Graphics 770 and Mesa 26.1.3, the PixelOE Vulkan stage gate passed 612 native dispatches. It covers numeric thresholds, discrete selection, iteration/centroid behavior and stable ties; its finite witness set is not a universal device arithmetic bound. The independent Torch comparison passed 40/40 fixtures with maximum absolute error 3.4570693969726562e-6 against the 1e-5 limit. Two separate public-port checks for `expanded` and `weight` also passed, with maximum errors 4.76837158203125e-7 and 5.364418029785156e-7. The validation-layer log is quiet. The records are [the stage gate](../../../out/gpu-whole-tiled/raw/pixeloe-vulkan-intel-stages-final.log), [the 40-case oracle](../../../out/gpu-whole-tiled/pixeloe-vulkan-oracle/validation.json), [the two public-port cases](../../../out/gpu-whole-tiled/pixeloe-vulkan-ports/validation.json), and [the validation-layer output](../../../out/gpu-whole-tiled/raw/pixeloe-vulkan-oracle-validation.log).

macOS Apple M5 Metal separately passed 598 native stage dispatches, its 40-case Torch comparison (maximum error 3.159046173095703e-6), and the CPU interval witness. These records are [the Metal stage gate](../../../out/gpu-whole-tiled/raw/pixeloe-native-interval-metal.log), [the Metal oracle](../../../out/gpu-whole-tiled/pixeloe-vulkan-metal-oracle/validation.json), and [the CPU witness](../../../out/gpu-whole-tiled/raw/pixeloe-native-interval-cpu.log). These checks preserve the Metal profile and do not establish Vulkan behavior.

A separate FreeBSD Intel paired experiment compared the prior three-stage GPU k-centroid reduction with the atomic GPU iteration. The runs used three alternating rounds, one warmup and eleven measured executions per configuration, with validation disabled. Median-of-round-medians changed from 15.6455 to 9.73507 ms at 128², 21.3699 to 15.7411 ms at 256², and 36.6713 to 35.5624 ms at 512². Dispatches/submissions fell from 27/28 to 19/20. Every paired output hash matched and peak managed bytes were unchanged. P95 spread was substantial, especially at 128² and 512², so the result is limited to this device, fixture and comparison; the small 512² change does not demonstrate a stable speedup. See the [raw paired data](../../../out/gpu-whole-tiled/raw/pixeloe-vulkan-kc-paired.jsonl).

PixelOE Vulkan has no controlled workload-size performance matrix yet. This k-centroid comparison does not measure overall plugin optimization or all options. NVIDIA hardware and Linux remain untested, while the CPU Whole table above and the CPU SIMD checks retain their existing CPU profile.

## Immutable coefficient tables and small stacks

`generate_tables.py` emits the coefficient tables as `constexpr` arrays and a sorted `constexpr` name table. `px::table(std::string_view)` returns a `TableView` containing a pointer and element count into those immutable plugin-image tables. Runtime lookup does not construct a per-call map or copy the coefficient arrays; each caller reads the same frozen values.

The regenerated set contains 279 tables and 116,149 FP32 values. The bitwise comparison found every value identical to the prior generated set, and the portable bundle regeneration check passed. With Clang `-O3 -fstack-usage`, the `px::table` static frame is 80 bytes for the `string_view` lookup path; the earlier owning-string/map path recorded 491,744 bytes. Static frame size describes compiler-reported stack use for that function; it is not a measurement of runtime RSS.

The focused small-stack test passed with four concurrent 128 KiB worker stacks on macOS and FreeBSD. The same table test passed under ThreadSanitizer on macOS, and the 3,720 SIMD/Slang stage comparisons passed on both hosts. These checks establish table identity and the small-stack execution path; they make no claim of a PixelOE algorithm runtime improvement. See the [table bit comparison](../../../out/gpu-whole-tiled/raw/pixeloe-table-bits.json), [before](../../../out/gpu-whole-tiled/tables-before.su) and [after](../../../out/gpu-whole-tiled/tables-after.su) stack-usage outputs, [macOS small-stack test](../../../out/gpu-whole-tiled/raw/pixeloe-tables-macos.log), [macOS SIMD comparison](../../../out/gpu-whole-tiled/raw/pixeloe-tables-simd-macos.log), [FreeBSD mixed-run checks](../../../out/gpu-whole-tiled/raw/mixed-smoke-freebsd.log), [macOS ThreadSanitizer test](../../../out/gpu-whole-tiled/raw/pixeloe-tables-tsan.log), and [bundle regeneration check](../../../out/gpu-whole-tiled/raw/pixeloe-tables-bundle.log). The stack measurement can be reproduced with [`reproduce_table_stack.py`](../../../out/gpu-whole-tiled/reproduce_table_stack.py); its [recorded output](../../../out/gpu-whole-tiled/raw/pixeloe-table-stack-repro.json) preserves compiler-reported frames and test results. The implementation is in [`runtime.hpp`](src/runtime.hpp) and [`generate_tables.py`](tools/generate_tables.py).
