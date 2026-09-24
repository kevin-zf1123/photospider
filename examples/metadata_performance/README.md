# FMT-08 CPU performance

Measured on 2026-09-24: Apple M5, arm64, macOS 27.2 (26B5091g), Homebrew
Clang 21.1.3, RelWithDebInfo (`-O2 -g -DNDEBUG`), one host CPU worker, fixed
128x128 DAG tiles, Float32, no sample-only result cache. Sources are provided
before timing; compilation and metadata preparation are timed separately.
Each case performs two warmups followed by seven serial repetitions. Each run
creates/retire its requested result; the input backing remains owned. p95 uses
sorted index floor((n-1)*0.95), so it is descriptive, not a tail guarantee.

Kernel images are always planar. Generic measurements below are additional
**non-image numeric tensor** checks, not an image representation or image fast path.
No codec/import conversion is timed. Profiling and ordinary timings are separate.
The accepted table was measured without a concurrent build. An earlier contended
run is excluded from conclusions.

## Planar public execution

`full` requests all HxWxC coordinates; `one` requests one full channel; `roi`
requests `[127,130) x [126,131) x channel 2`, crossing both 128 boundaries.
That ROI has 15 Float32 samples / 60 logical bytes.

| Size / channels | Order | Layout | Edit | Request | Prepare p50 (us) | Execute p50 (us) | Execute p95 (us) | Operation p50 (us) |
| --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: |
| 4096² / 4 | tiled | auto | patch | full | 75.88 | 527.33 | 528.42 | 265 |
| 4096² / 4 | tiled | view | patch | full | 77.62 | 586.00 | 602.58 | 293 |
| 4096² / 4 | tiled | materialize | patch | full | 74.38 | 19824.10 | 20321.20 | 19500 |
| 4096² / 4 | continuous | view | patch | full | 83.08 | 470.67 | 482.21 | 241 |
| 4096² / 4 | continuous | materialize | patch | full | 75.46 | 20032.70 | 21221.00 | 19749 |
| 4096² / 4 | tiled | materialize | patch | one | 78.00 | 4555.58 | 4824.67 | 4495 |
| 4096² / 4 | tiled | materialize | patch | roi | 79.29 | 19.62 | 21.29 | 18 |
| 512² / 32 | tiled | view | patch | full | 138.46 | 507.08 | 510.29 | 256 |
| 512² / 32 | tiled | materialize | patch | full | 147.38 | 3759.29 | 4117.92 | 3486 |
| 512² / 32 | tiled | view | replace | full | 167.25 | 514.96 | 596.88 | 269 |
| 512² / 32 | tiled | materialize | replace | full | 165.54 | 3853.08 | 4095.33 | 3580 |
| 512² / 32 | tiled | view | cascade | full | 124.50 | 516.83 | 529.04 | 268 |
| 512² / 32 | tiled | materialize | cascade | full | 132.46 | 3840.67 | 3940.08 | 3568 |

The named Apple Silicon profile additionally measured 497.83 us p50 / 502.38 us
p95 for 4096²x4 tiled view, and 19.04 / 20.96 us for materialized ROI. It uses the
same exact copy/metadata semantics; these independent timing samples do not
establish a different algorithm or ISA speedup. The x86-64 key is registered and
wrong-host admission is tested; no x86-64 timing is claimed.

## Optimization and profiler evidence

A CPU Profiler recording of the original 4096²x4 tiled copy contained 15,656
selected execution samples. `copy_spatial_channel_piece` had 14.53% self and
69.58% inclusive cycle weight; `_platform_memmove` had 45.66% self weight.
The optimized path coalesces adjacent rows only when both strides equal the
actual copied row width, with no padding or tile gap. Blocks remain at most
1024 samples, preserving stop/currentness checks and exact source support.
Partial-width/padded rectangles retain the row path. A second completed recording
contained 15,172 selected execution samples: copy-function self weight fell to
1.54%, while its inclusive weight remained 68.32% and `_platform_memmove` self
weight became 64.50%. This supports the reduced traversal/check overhead and
continued memory-copy bottleneck; inclusive weights overlap and cannot be summed.

Four alternating baseline/optimized pairs, each with two warmups and eleven
repetitions, measured median-of-run-medians **19.834 ms -> 19.446 ms** for tiled
4096²x4 materialization: about **2.0% lower public latency**. The same experiment
showed no benefit for continuous storage (19.290 -> 19.752 ms), so the final
implementation enables cross-row coalescing only for tiled storage. The small
tiled gain is machine/workload specific; page provisioning and actual memory copy
remain the main costs. Choosing a legal view eliminates sample copying but retains
its source; it is a layout choice, not equivalent memory ownership to materialize.

Generic numeric copying previously used a coordinate visit and host work check
per sample. Coalesced signed-stride runs, still checking every <=1024 samples,
reduced one 512²x4 full-copy comparison from **43.929 ms -> 4.517 ms** public and
39.834 ms -> 0.210 ms operation time. This auxiliary optimization does not permit
interleaved images inside the kernel. Public generic execution also computes its
normal result digest, explaining the public/core difference.

## Backing, coverage and accounting

The 4096²x4 source has 256 MiB of provided sample backing and a 256 MiB virtual
range. A full view keeps that same owner and copies **zero** sample bytes.
A full materialization creates 256 MiB new backing; a single-channel result
creates 64 MiB. The 60-byte crossing ROI creates only **64 KiB** of new page
backing, with exact 15-sample validity, while reserving the standard full-image
256 MiB virtual span. These byte quantities are not RSS.

For these fully provided sources, the reported live peaks were approximately
263.76 MiB (view), 525.26 MiB (full copy), 331.39 MiB (one-channel copy), and
263.83 MiB (ROI). They include retained source backing/metadata; input metadata
was 8,128,040 bytes. In the 32-channel cases, source/result backing is 32 MiB;
view shares it, copy provides another 32 MiB. Resource snapshots are tested by
ownership/budget fixtures; the performance input contains no ICC/config bytes,
so its resource-blob contribution is zero.

Planar `source_read_bytes` reports **declared source support**, including views;
it is not a count of actual CPU pixel loads. `result_copy_bytes` reports zero
for views and exact requested bytes for copies. Generic execution does not expose
these same per-copy counters; its zero diagnostic fields are N/A, not evidence
of zero copying. Owner identity, exact output coverage and independent bit checks
establish which path ran. Raw metadata/virtual/backing/peak values are in JSONL.

## Correctness and reproduction

The benchmark checks independently generated sample words at ROI corners and
checks the expected description. The integration fixture checks every requested
sample byte, including signaling NaNs, signed zero, integer extrema, rank 1..8,
negative/broadcast strides, both plane orders, padding/edges, sparse coverage,
resource/cancellation/work failures, source immutability and cascade semantics.
Both profiles preserve identical bytes. No timing is measured while under a debugger.

```sh
cmake --build build --target photospider_metadata_performance test_metadata_assignment -j 8
./build/test_metadata_assignment
python3 examples/metadata_performance/run.py \
  build/examples/metadata_performance/photospider_metadata_performance \
  build/fmt08-performance/accepted-results.jsonl
# size storage layout edit request repetitions channels profile
./build/examples/metadata_performance/photospider_metadata_performance \
  4096 tiled materialize patch roi 7 4 strict
```

The workload explicitly raises dependency/Footprint work fuel to 2^34 so large
numeric-tensor copies measure the algorithm rather than the default discovery
limit. Resource exhaustion remains a tested failure, not an approximation or
fallback. Compilation/preparation are outside per-observation scratch budgets,
matching the existing public preparation API.

The current workspace raw artifacts are `build/fmt08-performance/accepted-results.jsonl`,
`paired.jsonl`, `before-generic.json`, `after-generic.json`, `apple-view.json`,
`apple-roi.json` and the before/after `.trace`, `.xml`, `.hotspots.json` files.
The paired local binaries `metadata-row-copy` and `metadata-optimized` capture the
two compared copy implementations; they are validation artifacts, not installed
backends. Rerunning the commands above measures the final production path.

For an execution-only cycle profile (use a new output trace name):

```sh
/usr/bin/arch -arm64 xcrun xctrace record --template Blank \
  --instrument 'CPU Profiler' --time-limit 10s \
  --output build/fmt08-performance/profile.trace --launch -- \
  "$PWD/build/examples/metadata_performance/photospider_metadata_performance" \
  4096 tiled materialize patch full 1000
/usr/bin/arch -arm64 xcrun xctrace export \
  --input build/fmt08-performance/profile.trace \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="cpu-profile"]' \
  --output build/fmt08-performance/profile.xml
python3 examples/metadata_performance/summarize_trace.py \
  build/fmt08-performance/profile.xml
```

Require a completed/saved recording with no run issues and a nonempty exported
sample set. The summarizer includes public execute and metadata-callback stacks;
inclusive percentages overlap. It excludes source setup and benchmark oracle
stacks and is not a wall-clock measurement. Native C++ symbols were resolved;
platform/private allocations and deduplicated symbols remain profiler limits.
