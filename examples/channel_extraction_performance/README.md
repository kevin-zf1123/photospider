# FMT-01 CPU performance

This public workflow benchmarks `channel.extract_index_strict` and the native
Apple Silicon profile through compile/execute. It uses a four-plane source,
extracts plane 1, and checks every output sample with a separate integer-coordinate
byte oracle. FP32/FP64 inputs store exactly representable integers. Special floating
payloads and other dtypes remain covered by `test_channel_extraction`.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build --target photospider_channel_performance -j 8
python3 examples/channel_extraction_performance/run.py \
  build/examples/channel_extraction_performance/photospider_channel_performance \
  build/fmt01-performance/current
```

The matrix includes 128×128 and 4096×4096 FP32 continuous/tiled images,
4096×4096 UInt8/FP64 tiled images, a cross-tile 3×3 ROI, and the native accelerated profile (Apple Silicon or x86 AVX2).
Tiled means physical 128×128 tile-separated planes; full rows request the entire
plane in one execute, not 1024 separate execute calls. Only the ROI rows request
[127,130)×[127,130). `auto`, `view`, and `materialize` are separate cases.
Run the binary without the runner to choose an individual case:

```sh
build/examples/channel_extraction_performance/photospider_channel_performance \
  4096 tiled fp32 materialize 9 strict full
```

Arguments are size, storage, dtype, layout (or `all`), measured repetitions,
profile (`strict`, `accelerated_apple_silicon`, or `accelerated_x86_64`), and `full|roi`. Source generation/provision and compilation are outside
execute timing. One CPU worker, no result cache, retained fully provisioned input,
two initial executions excluded from steady-state timing. Each materialized result
uses fresh output backing and is destroyed outside its timed interval. Only the
first execution per layout is exhaustively validated. `first_us` includes first
execution but excludes compilation. Nine large-case samples and 31 small/ROI
samples are used. p95 is the lower empirical quantile `floor((n-1)*0.95)` and is
not a statistically stable tail-latency estimate. The harness logs individual
samples to stderr for subsequent runs.

`callback_p50_us` includes registry validation, output page preparation, the
operation callback, and commit. It is not an isolated memcpy measurement. View
has zero callback duration because it is handled by the executor; its public
execute latency is still measured. Output backed/virtual bytes for a view describe
its retained source owner, including unselected planes. `peak_live_bytes` is the
execution budget's modeled capacity, not process RSS. Source read bytes are exact
logical requested bytes, not hardware memory traffic.

## Xcode CPU Profiler

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

## Measured optimization, 2026-09-24

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

## Tiled overhead optimization, 2026-09-24

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

Native focused tests and both installed planar/channel consumer executables pass.
The rectangle regression exercises permuted axes, padded continuous rows, partial
edge tiles, ROI rejection, retained aliases and writer rollback. Non-power-of-two
tiles are rejection cases. Run the public fixture with:

```sh
cmake --build build --target test_planar_image_workflow test_channel_extraction -j 8
ctest --test-dir build -R '^(test_planar_image_workflow|test_channel_extraction)$' --output-on-failure
```

Both tests must exit zero after checking independent byte oracles and API bounds.

### x86 / AVX2 verification

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

These are current-code portability/performance measurements; no pre-change WSL
speedup or cross-machine performance comparison is claimed. No custom SIMD kernel
was added. The formal x86 accelerated entry point was executed on an AVX2 host.
Raw results, test/build logs and environment are copied to
`build/fmt01-performance/tile-opt/wsl/`. The remote checkout is
`/home/alex/photospider-fmt01-tile-20260924`, with build targets and suite runnable
using the same commands above (select `clang`/`clang++` during CMake configure).
