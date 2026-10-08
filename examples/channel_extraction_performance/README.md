# FMT-01 performance history and Result smoke

The benchmark source uses the public Result API. The measurements below are
historical records from the former Value/planar path and do not measure current
Result execution. Two bounded Result smoke scenarios passed their byte oracle;
no full Result performance matrix or timing conclusion is claimed.

## Current Result smoke

Build the current CLI and run the reported 128x128 full-plane and 130x130 tiled
ROI smoke cases with all three layout choices:

```sh
cmake --build build --target photospider_channel_performance -j 8
build/examples/channel_extraction_performance/photospider_channel_performance \
  128 continuous fp64 all 2 strict full
build/examples/channel_extraction_performance/photospider_channel_performance \
  130 tiled fp32 all 2 strict roi
```

Arguments are size, storage, dtype, layout, repetitions, CPU profile and
`full|roi`. Source creation, sample provision and compilation occur before
execute timing. An independent byte oracle checks the first output outside the
timed interval. These bounded runs are functional smoke checks, not a benchmark
matrix or a basis for speedup claims.

The CSV reports `source_logical_bytes` from the exact dependency support times
element width. `run_live_payload_bytes` and `run_live_metadata_bytes` are the
Root's live capacity above its post-source-setup baseline. `root_peak_payload_bytes`
and `root_peak_metadata_bytes` are cumulative same-context high-water marks that
include the source Result. The public Result API does not expose the operation's
own output-backed or virtual byte counts, so the CLI omits those old Value-path
metrics. Root capacity is not process RSS.

## Historical Value/planar profiling and measurements

The former benchmark used `run.py` to execute a 128x128/4096x4096 matrix across
continuous/tiled layouts, dtypes, views, materialization and ROI. Its CSV fields,
Xcode profiles and all timing tables below belong to that Value/planar executable.
They are retained as historical measurements only; do not apply them to the
current Result implementation.

### Historical Xcode CPU Profiler

```sh
/usr/bin/arch -arm64 /usr/bin/python3 \
  examples/channel_extraction_performance/profile.py \
  build/examples/channel_extraction_performance/photospider_channel_performance \
  build/fmt01-performance/current-cpu.trace
```

Use a fresh output name. This records a 10-second CPU Profiler launch and exports
`cpu-profile` XML and cycle-weighted self/inclusive hotspots. Only stacks containing
`execute_planar` or `invoke_planar` contribute; input setup and oracle are excluded.
Inclusive entries overlap and must not be summed. This script is macOS/Apple
Silicon specific; the native launcher avoids inherited Rosetta tool preferences.
The recorder now uses `--template Blank --instrument 'CPU Profiler'`. The standard
CPU Profiler template also includes Points of Interest, Hangs and Thermal State;
on this macOS 27 beta it reported a corrupt/incomplete log archive at shutdown.
Setting `Points of Interest.excludeOSLogs=true` alone did not fix it. Selecting
only the CPU instrument eliminated the error: the final recording exited 0,
reported completion, and exported 9,333 execution samples. This repairs the
profiling workflow; it does not establish that the OS log archive itself is repaired.
The additional log/hang/thermal timelines are omitted.

A time-limited CPU-only run also returned 54 after reporting completed/saved
recording without run issues. The script accepts only 0 or that observed code,
requires explicit completion/save messages and no reported run issues, and still
requires a successful export with nonempty execution samples. Recorder logs are
retained for inspection.

### Historical measured optimization

Apple M5, 10 logical CPUs, 32 GiB, macOS 27.0 (26A5425a), Clang 21.1.3,
RelWithDebInfo (`-O2 -g -DNDEBUG`), Xcode xctrace 27.0 (27A266a), 16 KiB pages.
Baseline was the FMT-01 implementation in the working tree on `ops-impl` based on
64b3100d, before the run-copy/page-span optimization. Both runs used the same
matrix, oracle, source ownership, worker count and timing boundaries. Timings are
native runs outside Instruments. Absolute values depend on host scheduling and
memory state; these are observed medians, not guarantees.

| Materialize workload | Before p50 | After p50 | Observed ratio |
| --- | ---: | ---: | ---: |
| 128² FP32 continuous | 430.834 µs | 37.375 µs | 11.53× |
| 128² FP32 tiled | 462.125 µs | 34.708 µs | 13.31× |
| 4096² FP32 continuous | 514.973 ms | 6.789 ms | 75.85× |
| 4096² FP32 tiled | 524.678 ms | 12.997 ms | 40.37× |
| 4096² UInt8 tiled | 492.914 ms | 4.640 ms | 106.24× |
| 4096² FP64 tiled | 546.825 ms | 20.510 ms | 26.66× |
| 4096² FP32 tiled, 3×3 ROI | 24.709 µs | 22.541 µs | 1.10× |
| 4096² FP32 tiled, Apple profile | 546.026 ms | 12.861 ms | 42.46× |

4096² FP32 view remained approximately 0.22–0.24 ms. Its work is coverage and owner
handling; materialize copies 64 MiB. The latter's measured modeled peak remained
345,115,840 bytes before/after, with 64 MiB output backing and 256 MiB retained
four-plane source backing. No memory saving is claimed.

Baseline CPU Profiler exported 16,509 execution-path samples. Coordinate-vector
construction was 27.384% inclusive, and `begin_write` was 25.804% inclusive, with
per-sample ordered-set page lookups and allocator activity prominent. The change
reuses coordinates, copies bounded contiguous row/tile runs, and builds the exact
page union from byte spans. Cancellation is checked within 1024 copied samples
and within 1024 page entries. No copy crosses window/tile authorization.

Afterward, 20,873 execution-path samples showed the per-pixel coordinate allocation
hotspot gone. `row_run` address/window work was 45.933% inclusive, `begin_write`
17.256% inclusive, and memcpy/memmove 12.927% self. These shares are conditional
cycle samples, not wall-time decomposition or additive percentages. Remaining
window/address overhead is a possible next optimization; this change preserves
the existing public bounded-window API.

Raw CSV summaries, recorder logs, exported XML and .trace bundles from this run
are in `build/fmt01-performance/`; generated traces are not source artifacts.
The failed Time Profiler attempts (`before.trace`, `before-native.trace`) are not
used for hotspot conclusions. `before-cpu.trace` and `after-cpu.trace` supply the
reported CPU evidence. Named lookup and split authoring were not separately timed.

### Historical tiled overhead optimization

Same M5/toolchain and timing policy as above. A fresh pre-change run is in
`tile-opt/before/summary.csv`; final serial measurements, taken after local builds
and Instruments had finished, are in `tile-opt/final-serial/summary.csv`.
Intermediate `rectangle`, `after`, and `final` directories are exploratory runs;
`final` overlapped a consumer build and is not the reported final timing series.

| Materialize workload | This round before p50 | Final p50 | Ratio |
| --- | ---: | ---: | ---: |
| 128² FP32 continuous | 39.125 µs | 35.583 µs | 1.10× |
| 128² FP32 tiled | 32.375 µs | 33.458 µs | 0.97× |
| 4096² FP32 continuous | 7228.710 µs | 5871.380 µs | 1.23× |
| 4096² FP32 tiled | 14795.500 µs | 6544.080 µs | 2.26× |
| 4096² FP32 tiled, 3×3 ROI | 21.250 µs | 22.375 µs | 0.95× |
| 128² FP64 continuous | 43.083 µs | 38.875 µs | 1.11× |
| 4096² U8 tiled | 4923.880 µs | 2057.580 µs | 2.39× |
| 4096² FP64 tiled | 23665.500 µs | 12056.600 µs | 1.96× |
| 4096² FP32 tiled, Apple profile | 14868.700 µs | 6259.670 µs | 2.38× |

The 4096² FP32 tiled/continuous ratio fell from 2.05 to 1.11; the absolute
extra latency fell from 7.567 ms to 0.673 ms (91% reduction). Small-image and tiny
ROI overhead is essentially unchanged at this sample count. No universal speedup
is claimed for those cases. FP32 materialize still retains 256 MiB source backing,
allocates 64 MiB output backing, and reports 345,115,840 modeled peak bytes.

Changes:

- `rectangle_run` certifies a bounded rectangle once, exposing per-row sample
  count and byte stride. FMT-01 traverses tile rectangles and reuses these checks
  for all rows; each copy still checks cancellation within 1024 samples.
- `begin_write` merges adjacent authorized full-width rows inside a tile before
  page-set lookup. Partial-width regions and padded rows remain separate spans;
  shared pages are deduplicated and cancellation is checked within 1024 pages.
- Tile geometry is now explicitly restricted to positive powers of two, with
  rejection at planning and image creation. Shift/mask addressing follows that
  checked contract. Image and ROI dimensions remain arbitrary positive extents.
- Copies use portable C++ and platform `memcpy`; no custom NEON/AVX2 assembly was
  introduced. The native accelerated profiles preserve identical byte results.

The intermediate rectangle-only CPU trace contained 7,773 execution samples;
page-set lookup was 30.344% self. After span merging and shift/mask addressing,
`final-cpu.trace` contained 9,333 execution samples: `_platform_memmove` was
58.224% self and `__mprotect` 2.611% self. Percentages are conditional cycle weights,
not elapsed-time fractions. CPU-only traces omit the auxiliary timelines of the
older template, so they are not an all-instruments comparison.

At the time, the Value/planar focused tests and installed consumers passed. The
rectangle regression exercised permuted axes, padded continuous rows, partial
edge tiles, ROI rejection, retained aliases and writer rollback. Those consumer
results are not installed-Result validation. The current Result integration test
is exercised separately below; non-power-of-two tiles remain rejection cases.

Run the current integration fixture with:

```sh
cmake --build build --target test_planar_image_workflow test_channel_extraction -j 8
ctest --test-dir build -R '^(test_planar_image_workflow|test_channel_extraction)$' --output-on-failure
```

Both tests check independent byte oracles and API bounds.

### Historical x86 / AVX2 verification

The same final C++ implementation was built in Ubuntu WSL2 on an Intel
Core i9-12900 (24 logical CPUs, AVX2 available), Clang 18.1.3, Linux
5.15.167.4-microsoft-standard-WSL2, 4 KiB pages, RelWithDebInfo. CPU workers,
cache, oracle, repetitions and timing boundaries match the native suite above.
Both focused integration tests passed (2/2); all 25 performance rows passed their
first-execution exhaustive byte oracle, including `accelerated_x86_64`.

| WSL materialize workload | p50 |
| --- | ---: |
| 128² FP32 continuous | 85.198 µs |
| 128² FP32 tiled | 98.509 µs |
| 4096² FP32 continuous, strict | 12.896 ms |
| 4096² FP32 tiled, strict | 13.810 ms |
| 4096² FP32 tiled, accelerated_x86_64 | 12.731 ms |
| 4096² UInt8 tiled | 3.733 ms |
| 4096² FP64 tiled | 25.944 ms |
| 4096² FP32 tiled, 3×3 ROI | 58.240 µs |

These are historical Value/planar portability measurements; they are not
current-Result timings. No pre-change WSL speedup or cross-machine performance
comparison is claimed. No custom SIMD kernel was added. The formal x86 accelerated
entry point was executed on an AVX2 host.
Raw results, test/build logs and environment are copied to
`build/fmt01-performance/tile-opt/wsl/`. The remote checkout is
`/home/alex/photospider-fmt01-tile-20260924`, with build targets and suite runnable
using the same commands above (select `clang`/`clang++` during CMake configure).
