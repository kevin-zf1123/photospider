# Codec and planar import results: 2026-09-24

Fixture display names below match `assets/codec/manifest.json`. Measurements
are unchanged; archived result JSON retains the original paths and names.

Measured on Apple M5, 32 GiB RAM, arm64 macOS, Clang 21, C++17, RelWithDebInfo (`-O2 -g -DNDEBUG`), Node 24.10.0, Sharp 0.35.4 and libvips 8.18.6. Kernel starting revision: `3a37a0e48915bf011e8e6f0822d10b4e465e7e3a`. WebUI had pre-existing uncommitted changes; the baseline used that working tree codec, before extraction/normalization changes. No package installs were performed for these measurements.

The comparison fixes Sharp concurrency to one, disables its operation cache, uses warm OS file caches, and collects two warmups plus seven measurements. Layouts are both FP32. Times below are medians in milliseconds. See [README.md](README.md) for precise timing boundaries.

| Image | Dimensions | Decode before → after | Continuous import before → after | Tile 128 import before → after | Tile speedup |
|---|---:|---:|---:|---:|---:|
| stage_rgba8_3504x4958.tiff | 3504 × 4958 | 278.65 → 277.33 | 746.89 → 29.08 | 830.62 → 35.69 | 23.28× |
| stage_cmyk8_4299x6071.tiff | 4299 × 6071 | 765.22 → 195.89 | 1120.18 → 46.04 | 1148.07 → 57.55 | 19.95× |
| illustration_rgba8_4962x5454.png | 4962 × 5454 | 615.37 → 606.54 | 1178.31 → 50.20 | 1217.63 → 55.26 | 22.04× |
| illustration_rgb8_4962x5454.jpeg | 4962 × 5454 | 180.77 → 176.82 | 1143.30 → 51.16 | 1223.79 → 55.20 | 22.17× |
| black_rgb8_2048x2048.png | 2048 × 2048 | 22.09 → 21.65 | 183.38 → 7.14 | 184.20 → 9.83 | 18.74× |
| white_rgb8_2048x2048.tiff | 2048 × 2048 | 22.97 → 22.70 | 179.41 → 7.12 | 190.28 → 10.14 | 18.76× |
| solid_blue_rgb8_2048x2048.jpeg | 2048 × 2048 | 23.44 → 21.79 | 192.93 → 7.35 | 190.23 → 10.21 | 18.62× |
| pattern_rgba16_129x131.png | 129 × 131 | 0.80 → 0.74 | 0.74 → 0.05 | 0.81 → 0.07 | 11.15× |

The stage-median sums below are estimates of decode plus import work, not direct end-to-end measurements; the benchmark excludes raw-file staging, IPC, thumbnail generation, persistence and workflow execution.

| Original image | Continuous sum before → after | Tile 128 sum before → after | Final decode / continuous / tile p95 |
|---|---:|---:|---:|
| stage_rgba8_3504x4958.tiff | 1025.54 → 306.41 | 1109.27 → 313.01 | 321.89 / 29.69 / 37.36 |
| stage_cmyk8_4299x6071.tiff | 1885.40 → 241.93 | 1913.29 → 253.45 | 237.40 / 48.42 / 180.87 |
| illustration_rgba8_4962x5454.png | 1793.68 → 656.74 | 1833.00 → 661.80 | 615.27 / 51.12 / 56.78 |
| illustration_rgb8_4962x5454.jpeg | 1324.07 → 227.99 | 1404.56 → 232.02 | 187.20 / 52.30 / 55.75 |

Seven-sample p95 is the maximum observed sample. Small differences in unchanged RGB decode are ordinary measurement variation; no RGB decompressor improvement is claimed. CMYK decode including normalization improved by 3.91×.

## Changes and profiling

1. `PlanarImage::import_value` now visits bounded physical rectangles and computes checked source/target addresses once per rectangle. Fixed-width row copies replace per-sample address validation and dynamic-size copies. Contiguous source rows use `memcpy`; arbitrary signed/broadcast strides use bit-preserving gathers. Each row contains at most 4096 samples before the next cancellation check. Allocation, budget admission, publication and rollback retain the existing transaction.
2. Host decode is extracted into `server/image-codec.ts`, reused by asset import and the benchmark. Aligned little-endian CMYK samples normalize through a zero-copy `Float32Array` view; the original Buffer path remains for other alignment/endian cases. Color transforms and ICC handling are unchanged.

Separate 8-second Instruments CPU Profiler recordings of the large PNG tiled import filtered to `PlanarImage::import_value` stacks:

- Before: 26,205 sampled rows; source address calculation accounts for 47.35% of self cycle weight.
- Final: 19,711 sampled rows; source address calculation falls to 0.186%; the fixed-width row-copy code accounts for 86.801%. These are sampled instruction/inline-frame attributions, not a proof of memory-bandwidth saturation or page-fault causality.
- Profiler timings are excluded from latency tables; inclusive percentages overlap and are not summed. The first intermediate tiled optimization still spent 9.901% in source addressing; moving that work to rectangle starts reduced it further.

## Memory

| Image | Logical FP32 MiB | Continuous backed MiB | Tile 128 backed MiB | Tile metadata MiB |
|---|---:|---:|---:|---:|
| stage_rgba8_3504x4958.tiff | 265.09 | 265.12 | 271.25 | 6.21 |
| stage_cmyk8_4299x6071.tiff | 398.24 | 398.25 | 403.75 | 8.44 |
| illustration_rgba8_4962x5454.png | 412.94 | 413.00 | 416.81 | 8.21 |
| illustration_rgb8_4962x5454.jpeg | 412.94 | 413.00 | 416.81 | 8.21 |

Page backing, virtual reservation and metadata accounting match the baseline for every case. Larger tiled spans reflect row/tile/page alignment. In a separate native-process RSS run for the 4962 × 5454 PNG tiled import, maximum RSS was 840.23 MiB before and 840.27 MiB after. This includes the interleaved input and fresh planar output; it excludes the Node decoder process. These optimizations remove computation overhead, not the interleaved staging allocation.

## Validation and reproduction artifacts

- Kernel: `test_planar_import` and `test_planar_image_workflow`, 2/2 passed after the final implementation. New coverage spans 7 scalar types × 6 axis permutations × 6 stride layouts × 2 destination orders (504 cases), plus rank-two, large-tile/chunk, budget and cancellation cases. Existing tests preserve NaN payloads, signed zero and infinities.
- Host: `tests/codec.test.ts` and `tests/assets.test.ts`, 5/5 passed; TypeScript check passed. All 256 UInt8 and 65,536 UInt16 CMYK codes are checked against `Math.fround(code / max)`. RGB16 and alpha use an independent transfer-function oracle across 128-pixel edges.
- All eight before/final decoded buffers are byte-identical, and all 16 final native layout cases compare every sample with the decoded source. ClangFormat 21, cpplint and diff whitespace checks passed.
- Independent scoped code review found no blocker/required findings, including the rectangle arithmetic refinement. Runtime cancellation during copying, big-endian execution, sanitizers and other platforms were not tested.

Ignored local artifacts under `build/codec-performance/`: `before/results.json`, `final/results.json`, their environment files, `before.trace`, `final.trace`, exported profile XML and `*-hotspots.json`, native `*-rss.txt`, and the baseline binary `before-native`. The archived final `.f32` files retain numeric names for the profiler command below; new benchmark runs use the fixture filenames described in README. Source images remain untouched.

For a comparable profiler recording (run from the kernel checkout):

```sh
xcrun xctrace record --template Blank --instrument "CPU Profiler" \
  --time-limit 8s --output build/codec-performance/recheck.trace \
  --launch -- "$PWD/build/examples/codec_performance/photospider_codec_performance" \
  "$PWD/build/codec-performance/final/2.f32" 4962 5454 tiled 1000
```

## Current host concurrency check

The installed Sharp default is four workers. A supplementary final-only run with `CODEC_CONCURRENCY=4` (still with the operation cache disabled) measured the following absolute latencies. These are not used in the one-worker before/after speedup calculation.

| Image | Decode p50 ms | Continuous import p50 ms | Tile 128 import p50 ms |
|---|---:|---:|---:|
| stage_rgba8_3504x4958.tiff | 99.72 | 28.88 | 35.25 |
| stage_cmyk8_4299x6071.tiff | 185.18 | 46.12 | 53.45 |
| illustration_rgba8_4962x5454.png | 390.66 | 50.80 | 55.73 |
| illustration_rgb8_4962x5454.jpeg | 71.26 | 51.88 | 55.80 |

A single CMYK tiled sample in the primary final run took 180.865 ms; it is retained in the p95 table. Its cause was not established. A separate 21-sample native recheck on the same final bytes measured p50 54.14 ms, p95 56.74 ms and max 58.94 ms (`build/codec-performance/cmyk-tail-recheck.json`). This recheck does not replace the primary samples.
