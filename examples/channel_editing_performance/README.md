# FMT-03 CPU performance and optimization

Follow-up: the [FMT-02 regression check](../channel_assembly_performance/FMT03-regression.md)
separated scalar fill from the established spatial copy loop. All 45 FMT-03
cases were rerun successfully; current results are in
`build/fmt03-performance/after-fmt02-regression/summary.csv`. The fill/4096/tiled
p50 is now 19.829 ms. The tables and CPU traces below retain the original
pre-isolation delivery measurements, so their revision remains explicit.

Measured 2026-09-24 on Apple M5 (10 logical CPUs, 32 GiB), macOS 27.2
(26B5091g), Clang 21.1.3, C++17 RelWithDebInfo (`-O2 -g -DNDEBUG`). Xcode
xctrace is 27.2 (27B5019j); page size is 16 KiB. The implementation uses one
configured CPU worker and the DAG-wide 128x128 tile geometry. No private worker
pool, completed-result cache, or custom ISA copy kernel is used. These are local
macOS CPU measurements; x86/WSL, native Windows and GPU were not measured here.

The latency and memory tables below record earlier Value/planar execution and
are historical measurements, not Result ABI 2 timing or memory evidence. FMT-03A/B
are current public authoring helpers over the registered FMT-02C Result key.
Current Result usage and metric interpretation are documented separately below.


## Current Result driver

The driver now runs FMT-03 public helpers through the registered FMT-02C Result
operation and the public `WorkflowDocument`, `Compiler` and `ExecutionContext`
APIs. It keeps the argument order `size member storage request layout
repetitions profile`. For example:

```sh
cmake --build build --target photospider_channel_editing_performance -j 8
./build/examples/channel_editing_performance/photospider_channel_editing_performance \
  130 fill tiled roi materialize 1 strict
```

Members A, B, repeat, fill, subset, insert and identity retain their documented
source and output mappings. The program checks requested samples against an
independent byte oracle. It uses one CPU worker and no custom thread pool.

`source_logical_bytes` sums `Footprint.element_count() * sizeof(float)` over
all bound inputs in the public `run.dependencies.source_support()` map. Both
drivers use Float32 sources, so it measures exact declared logical support, not
physical I/O or output copy traffic. `source_payload_bytes` and `source_metadata_bytes` report Root live
payload and metadata after source setup, before benchmark runs.
`run_live_payload_bytes` and `run_live_metadata_bytes` are live-resource deltas
above that baseline while the current execution result remains alive. They cover
all resources retained under the Root at the sampling point, not output storage
alone. `root_peak_payload_bytes` and `root_peak_metadata_bytes` report cumulative
Root lifetime peaks. They include source setup and earlier runs, and the first-run
oracle window can contribute; they are not per-run peaks. These managed resource
values are not RSS. The CSV columns are `size`, `dtype`, `member`,
`storage`, `request`, `layout`, `profile`, `repetitions`, `compile_us`,
`first_us`, `p50_us`, `p95_us`, `core_p50_us`, `source_logical_bytes`,
`source_payload_bytes`, `source_metadata_bytes`, `run_live_payload_bytes`,
`run_live_metadata_bytes`, `root_peak_payload_bytes`, and
`root_peak_metadata_bytes`. The public Result API does not expose output virtual
reservation or a separate copied-byte counter.

The latency tables, profiler traces and memory tables below are historical
Value/planar-driver measurements. Fields such as `copied_bytes`, `output_virtual`
and modeled-output totals describe that driver; they are absent from the current
Result CSV.


### Current Result smoke coverage

The current Result driver was built and run in 11 small strict-profile smoke
cases, each with one measured repetition after two warmups. Every case passed
the independent byte oracle. This is focused execution evidence, not the former
full performance matrix or profiling evidence.

| Member | Smoke configuration |
| --- | --- |
| Editing A | `128 A tiled full materialize` |
| Editing B | `128 B continuous one materialize` |
| Repeat | `128 repeat continuous full auto` |
| Fill | `130 fill tiled roi materialize` |
| Identity | `128 identity tiled full view` |
| Subset | `130 subset tiled roi auto` |
| Insert | `128 insert continuous one materialize` |

The other four cases cover FMT-02 assembly and shared-owner view and are listed
in its [performance guide](../channel_assembly_performance/README.md). FMT-03
fill ROI reports 112 logical source bytes and subset ROI reports 72. Identity
reports zero additional live payload at the observation point. No 4096x4096
Result run, complete matrix, or new Result profile has been measured.


## Full Result matrix

The standalone example target also builds against the installed public kernel package:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
cmake -S examples/channel_editing_performance -B build/channel-editing-performance-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/channel-editing-performance-consumer --target photospider_channel_editing_performance -j 8
```

The installed-package target builds successfully. The following command runs the
current Result matrix. Only the smoke cases above were run for this update; the
full matrix and profiler were not run.

```sh
cmake --build build --target photospider_channel_editing_performance -j 8
python3 examples/channel_editing_performance/run.py \
  build/examples/channel_editing_performance/photospider_channel_editing_performance \
  build/fmt03-performance/current
```

The binary accepts `size member continuous|tiled full|one|roi
layout repetitions profile`. `member` is A, B, repeat, fill, subset, insert or
identity. All workloads are **FP32**. The 45-case serial matrix includes the
required 128x128, 4096x4096 continuous and 4096x4096 tiled cases, strict and native
Apple Silicon profiles, materialize/auto/view, single-channel and cross-tile ROI.
Each process checks every requested first-execution output byte against an
independent coordinate oracle, outside the timing interval.

| Member | Source and output mapping |
| --- | --- |
| A | RGBA base -> `[2,1,0,3]` (BGR reorder, alpha retained). |
| B | RGBA base plus independent plane and scalar -> `[external,base1,base2,0.5]`. |
| repeat | RGBA -> `[0,0,2,3]`, testing reuse and omitted source channel 1. |
| fill | RGBA -> `[2,1,0,0.5]`, with explicit connected scalar. |
| subset | RGBA -> `[0,0,2]`, changing output channel count and reusing source 0. |
| insert | RGB -> `[2,1,0,1.0]`, inserting an opaque constant alpha slot. |
| identity | RGBA -> `[0,1,2,3]`, common-owner view/auto/materialize comparison. |

`one` selects scalar output channel 3 for B/fill/insert, otherwise output channel
0. `roi` requests all output channels in y/x=[127,130), crossing four 128x128
tiles. `full` requests the complete output. All axes are HWC with channel axis 2.
Continuous rows have width*4 byte pitch; tiled rows have 512 byte pitch. Source
samples are fully provisioned and retained before timing. Each benchmark expands
to one C node and zero literal providers; one connected scalar declaration is
present (unused for A/repeat/subset/identity). Sources plus literal providers and
rank/axis variants receive separate integration coverage.

Source setup and compilation are excluded from execute latency and reported
separately. Two initial executions are warmups. Large cases have nine measured
samples; 128x128 and ROI cases have 31 (identity `one` also has 31). Every
materialized execution gets fresh output backing; destruction occurs after the
timer. p95 is the lower empirical order statistic `floor((n-1)*.95)`, a small
sample statistic rather than a tail-latency guarantee. `core_p50_us` measures the
mapped operation's host work, including mapping, page preparation, copy/fill and
publication. Public execution also includes bindings, scheduling and admission.
Raw per-execution samples are in each case's `.log`; CSV includes compile/first
latency, source/copied bytes, backing, virtual span, metadata and modeled peak.

## Historical Value/planar serial results

All 45 cases pass. Latencies are **microseconds**, p50 / p95.

| Size | Member | Storage | Request | Layout/profile | p50 / p95 (us) |
| --- | --- | --- | --- | --- | ---: |
| 128² | A | continuous | full | materialize/strict | 59.167 / 67.375 |
| 4096² | A | continuous | full | materialize/strict | 19812.400 / 20380.400 |
| 128² | A | tiled | full | materialize/strict | 55.292 / 57.625 |
| 4096² | A | tiled | full | materialize/strict | 19978.000 / 20615.200 |
| 4096² | A | tiled | one | materialize/strict | 4434.420 / 4615.250 |
| 4096² | A | tiled | roi | materialize/strict | 25.625 / 29.875 |
| 4096² | A | tiled | full | auto/strict | 19635.400 / 20135.100 |
| 4096² | A | tiled | full | materialize/native | 19945.100 / 21784.000 |
| 128² | B | continuous | full | materialize/strict | 56.583 / 58.417 |
| 4096² | B | continuous | full | materialize/strict | 19211.200 / 19713.000 |
| 128² | B | tiled | full | materialize/strict | 54.375 / 58.833 |
| 4096² | B | tiled | full | materialize/strict | 20455.400 / 20808.800 |
| 4096² | B | tiled | one | materialize/strict | 5870.000 / 6014.960 |
| 4096² | B | tiled | roi | materialize/strict | 26.333 / 31.000 |
| 4096² | B | tiled | full | auto/strict | 21517.800 / 22274.400 |
| 4096² | B | tiled | full | materialize/native | 20730.000 / 20859.000 |
| 128² | fill | continuous | full | materialize/strict | 56.500 / 64.209 |
| 4096² | fill | continuous | full | materialize/strict | 19622.000 / 19891.300 |
| 128² | fill | tiled | full | materialize/strict | 56.250 / 60.083 |
| 4096² | fill | tiled | full | materialize/strict | 20541.100 / 21125.000 |
| 4096² | fill | tiled | one | materialize/strict | 5362.580 / 5398.000 |
| 4096² | fill | tiled | roi | materialize/strict | 27.958 / 32.875 |
| 4096² | fill | tiled | full | auto/strict | 20911.100 / 21342.800 |
| 4096² | fill | tiled | full | materialize/native | 21038.400 / 21395.300 |
| 128² | repeat | continuous | full | materialize/strict | 54.750 / 59.750 |
| 4096² | repeat | continuous | full | materialize/strict | 18872.500 / 19232.000 |
| 128² | repeat | tiled | full | materialize/strict | 56.291 / 70.708 |
| 4096² | repeat | tiled | full | materialize/strict | 19596.700 / 19936.200 |
| 4096² | repeat | tiled | one | materialize/strict | 4701.210 / 4741.670 |
| 4096² | repeat | tiled | roi | materialize/strict | 29.417 / 33.208 |
| 4096² | repeat | tiled | full | auto/strict | 19985.400 / 20132.500 |
| 4096² | repeat | tiled | full | materialize/native | 19939.800 / 20184.400 |
| 4096² | identity | tiled | full | view/strict | 473.792 / 482.542 |
| 4096² | identity | tiled | full | auto/strict | 475.916 / 480.417 |
| 4096² | identity | tiled | full | materialize/strict | 19957.200 / 20480.900 |
| 4096² | identity | tiled | one | view/strict | 101.417 / 108.209 |
| 4096² | identity | tiled | roi | view/strict | 4.167 / 5.209 |
| 128² | subset | continuous | full | materialize/strict | 42.125 / 49.125 |
| 4096² | subset | tiled | full | materialize/strict | 14505.800 / 14805.500 |
| 4096² | subset | tiled | one | materialize/strict | 4789.960 / 5082.460 |
| 4096² | subset | tiled | roi | materialize/strict | 21.417 / 24.584 |
| 128² | insert | continuous | full | materialize/strict | 53.625 / 60.709 |
| 4096² | insert | tiled | full | materialize/strict | 20609.300 / 21426.300 |
| 4096² | insert | tiled | one | materialize/strict | 5306.500 / 5375.750 |
| 4096² | insert | tiled | roi | materialize/strict | 25.750 / 38.333 |

## Historical Value/planar optimization evidence

Before optimization, fill/4096/tiled/full/materialize had **49.973 ms p50**
(49.704 ms core) over five samples. The final matching case has **20.541 ms p50**
(20.263 ms core), p95 **21.125 ms** over nine samples: approximately **2.43x**
lower median latency, or **58.9%** less time. The small B/128/continuous pilot
moved from 83.416 us (nine samples) to 56.583 us (31 samples). The larger fill
case is the primary performance comparison; no cross-platform speedup is claimed.

The initial implementation already fused scalar fills into C's exact dependency
mapping. Profiling then identified per-sample tiny memcpy calls: before, execution
stacks contained 29,902 CPU samples, with `_platform_memmove` 59.310% self,
`DYLD-STUB$$memcpy` 16.967%, and copy traversal 12.112%. The optimized path:

1. Repeats exact represented bytes using nonoverlapping doubling copies inside
   blocks of at most 1024 samples, preserving NaNs and integer endpoints.
2. Traverses the entire authorized output rectangle for a fixed-coordinate scalar,
   avoiding redundant per-row source/window lookup. Spatial generic sources keep
   their bounded single-row traversal and signed/zero-stride handling.

No spatial temporary or mutable constant cache is introduced. Scalar Data remains
one sample even for full-plane output; dirty mapping fans out only to selecting
slots. Tile/ROI boundaries constrain writes, work/page budgets remain admitted,
and cancellation/currentness checks remain at most 1024 samples apart with a
final atomic publication. Integration tests verify forced generic zero-stride
views, independent-owner fallback, canonical image rejection, sparse coverage,
all seven dtypes and rank 1..8.

The final CPU trace has 16,880 execution samples: `_platform_memmove` 47.085%
self, copy traversal 16.714%, and memcpy stub 3.933%. Percentages are conditional
cycle weights in execution stacks, not wall-time decompositions; inclusive stacks
would overlap. Profiler timings are excluded from the benchmark. Remaining costs
include actual output copy/fill, fresh-page provisioning and coverage/admission.
No in-flight cancellation-latency or OS page-fault optimization claim is made.

```sh
python3 examples/channel_assembly_performance/profile.py \
  build/examples/channel_editing_performance/photospider_channel_editing_performance \
  build/fmt03-performance/new-fill.trace \
  4096 fill tiled full materialize 1000 strict
```

The shared profiling script records CPU Profiler only, requires a fresh trace
path, exports execution-stack cycle samples and rejects empty/failed recordings.
Both before/after recordings completed without reported run issues. Compiler
activity outside the profiled process is not included in the filtered stacks.

## Historical Value/planar memory and exact support

These are controlled capacities, not RSS or memory-bus traffic. Every case keeps
its complete source allocation alive. Materialized output virtual span is the
whole logical image, while backing follows requested pages. Values in MiB use
2^20 bytes; the CSV preserves exact integers.

| 4096² workload | Logical source / copied bytes | Output backing | Output virtual | Modeled peak |
| --- | ---: | ---: | ---: | ---: |
| fill/full/materialize | 201,326,596 / 268,435,456 | 256.000 MiB | 256 MiB | 525.267 MiB |
| B/full/materialize | 201,326,596 / 268,435,456 | 256.000 MiB | 256 MiB | 590.643 MiB |
| B/one/materialize | 4 / 67,108,864 | 64.000 MiB | 256 MiB | 396.768 MiB |
| B/roi/materialize | 112 / 144 | 0.250 MiB | 256 MiB | 329.401 MiB |
| subset/full/materialize | 134,217,728 / 201,326,592 | 192.000 MiB | 192 MiB | 459.889 MiB |
| identity/full/view | 268,435,456 / 0 | 256.000 MiB | 256 MiB | 263.765 MiB |

B retains 320 MiB source pixels (base plus independent replacement plane); fill
retains 256 MiB, plus four scalar bytes. Scalar-only requests read four logical
source bytes but retain the bound sources. The B ROI reads 112 source bytes and
produces 144 logical bytes while backing 256 KiB, due to four tiles across four
output planes. The unrequested samples in those pages are not valid. Identity
views copy zero bytes and retain the original 256 MiB owner. Optimization changes
copy traversal, not output backing or lifetime accounting.

## Historical Value/planar validation and artifacts

Seven focused CTests pass: channel editing, assembly, extraction, planar workflow,
compiler, Value and execution demand. The minimal public example checks the offset
ROI `[8,21,31,0.5]`. The FMT-03 integration source also builds and passes using
`find_package` against `build/fmt03-install`, via `photospider_channel_editing_consumer`.
ClangFormat 21, cpplint and diff whitespace checks pass. Independent spec and code
subagents reviewed the implementation, and confirmed required fixes for group
retention, actual literal-node counting, raw-axis metadata safety and invalid
dtype rejection. No outstanding blocker/required finding remains.

Raw results are `build/fmt03-performance/final-native-45/summary.csv` and its
per-case `.csv`/`.log` files. Before data is `before-fill.csv/.log` and
`before-small.csv/.log`. CPU traces, exported XML and hotspot JSON are
`before-fill.*` and `after-fill.*` under `build/fmt03-performance/`.
The earlier `final-native/` matrix contains the first 37-case pass; final delivery
uses the 45-case directory. Generated traces and output remain ignored build
artifacts. No full CTest, sanitizer or other-platform validation is claimed.
