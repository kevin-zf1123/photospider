# FMT-02 CPU performance and profiling

Package 0.21.0, measured 2026-09-24 from the `ops-impl` working tree based on
`3f645879710f496af4809356591d3403e0820761`. FMT-02 A/B/C are implemented; Proposed
specification decision status is unchanged. The public source fixture and
registry/helper contract are in [Channel and color operations](../../docs/kernel-architecture/Channel-and-Color-Operations.md#fmt-02-channel-assembly).

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build --target photospider_channel_assembly_performance -j 8
python3 examples/channel_assembly_performance/run.py \
  build/examples/channel_assembly_performance/photospider_channel_assembly_performance \
  build/fmt02-performance/current
```

The binary accepts `size A|B|C|view continuous|tiled full|one|roi
layout repetitions profile`. All workloads are **Float32**. `A` assembles four
independently owned component planes, `B` concatenates RGB plus alpha, and `C`
maps one RGBA input as [2,0,2,3], reusing one source and omitting channel 1.
`view` runs FMT-01 split followed by FMT-02 assembly from a common root owner.
`one` selects output channel 2. `roi` requests all four output channels in
[127,130) x [127,130), crossing four 128x128 tiles. ROI requires size >=130.

The 24-case matrix explicitly includes FP32 128x128 continuous/tiled and
4096x4096 continuous/tiled for A and B, full/single-channel/ROI requests,
unrelated-owner auto, native accelerated profiles, mapped reuse, and
common-owner view/auto/materialize. Every case checks every first-execution
output sample with an independent coordinate/byte oracle outside timing.
Unit/integration fixtures additionally cover all seven dtypes, NaN/signed-zero
bits, negative/zero strides, sparse demand and metadata/error cases.

One configured CPU worker, CPU backend, no completed-result cache. Sources are
fully provisioned and retained before timing. Source generation, initial page
provision and compile are separate from execute. Each materialized execution
uses fresh result backing; result destruction occurs outside its interval.
Two initial executions are excluded. Large cases have 9 measured samples;
128x128 and small ROI cases have 31. p95 is the lower empirical order statistic
`floor((n-1)*0.95)`, not a stable tail-latency guarantee. Raw samples remain in
per-case `.log` files. The original scalar-copy baseline had 3 measured samples.

`core_p50_us` is the assembly's own host work, including mapping, view proof,
page preparation/copy/commit, excluding recursive producer execution; it is
not an isolated memcpy measurement. Public execute also includes bindings,
resource accounting, scheduling and any split/source subplans. No private
worker pool or custom ISA copy kernel is used.

## Machines and build

| Host | Environment |
| --- | --- |
| macOS | Apple M5, 10 CPUs, 32 GiB; macOS 27.2 (26B5091g); Clang 21.1.3; xctrace 27.2 (27B5019j); 16 KiB pages. |
| Linux/x64 | `ssh winpc`, Ubuntu WSL2; Intel Core i9-12900, 24 logical CPUs, AVX2; Linux 5.15.167.4-microsoft-standard-WSL2; Clang 18.1.3; 4 KiB pages. |

Both use RelWithDebInfo (`-O2 -g -DNDEBUG`), C++17, one configured worker and
the same workflow/oracle. Native accelerated means Apple Silicon on macOS and
x86_64 on WSL. Tiled physical rows have pitch 512 bytes; continuous FP32 rows
have pitch width*4 bytes. Physical tile geometry remains 128x128 in the DAG.
These are measurements of the two hosts, not a controlled CPU comparison.

## Final serial latency

All values below are **microseconds**, shown as p50 / p95. FP32 is explicit in
the raw CSV `dtype` column. All 24 cases passed on each host.

| Size | Member | Storage | Request | Layout/profile | M5 p50 / p95 (us) | WSL p50 / p95 (us) |
| --- | --- | --- | --- | --- | ---: | ---: |
| 128² | A | continuous | full | materialize/strict | 54.875 / 60.291 | 105.014 / 115.931 |
| 4096² | A | continuous | full | materialize/strict | 18395.300 / 18864.600 | 46182.800 / 47485.100 |
| 128² | A | tiled | full | materialize/strict | 55.083 / 62.750 | 102.753 / 116.453 |
| 4096² | A | tiled | full | materialize/strict | 18832.800 / 30078.200 | 49091.300 / 50328.900 |
| 4096² | A | tiled | one | materialize/strict | 4319.120 / 4506.830 | 11689.600 / 11894.400 |
| 4096² | A | tiled | roi | materialize/strict | 25.750 / 31.083 | 30.626 / 53.455 |
| 4096² | A | tiled | full | auto/strict | 18438.100 / 18695.800 | 47566.200 / 49073.600 |
| 4096² | A | tiled | full | materialize/native | 18370.600 / 18780.400 | 46135.200 / 49100.100 |
| 128² | B | continuous | full | materialize/strict | 53.167 / 58.666 | 93.096 / 104.741 |
| 4096² | B | continuous | full | materialize/strict | 18911.100 / 19368.200 | 46866.300 / 47680.600 |
| 128² | B | tiled | full | materialize/strict | 51.875 / 59.416 | 93.393 / 102.575 |
| 4096² | B | tiled | full | materialize/strict | 18517.500 / 18763.300 | 48168.800 / 52079.500 |
| 4096² | B | tiled | one | materialize/strict | 4472.500 / 4590.170 | 11849.900 / 12295.500 |
| 4096² | B | tiled | roi | materialize/strict | 27.042 / 32.417 | 26.041 / 32.302 |
| 4096² | B | tiled | full | auto/strict | 18783.800 / 19732.800 | 45665.900 / 48017.200 |
| 4096² | B | tiled | full | materialize/native | 18936.200 / 19141.500 | 46295.300 / 46768.700 |
| 128² | C | tiled | full | materialize/strict | 54.000 / 57.583 | 101.603 / 109.815 |
| 4096² | C | tiled | full | materialize/strict | 18361.000 / 19183.600 | 46360.000 / 47532.500 |
| 4096² | C | tiled | roi | auto/strict | 29.458 / 44.709 | 28.279 / 34.156 |
| 4096² | view | tiled | full | view/strict | 1555.960 / 1567.920 | 1582.930 / 1653.150 |
| 4096² | view | tiled | full | auto/strict | 1635.750 / 1834.750 | 1622.150 / 1654.820 |
| 4096² | view | tiled | full | materialize/strict | 20008.600 / 20332.000 | 47813.000 / 48608.200 |
| 4096² | view | tiled | one | view/strict | 384.334 / 402.917 | 389.207 / 506.596 |
| 4096² | view | tiled | roi | view/strict | 48.125 / 51.667 | 98.583 / 118.498 |

The macOS A/tiled/full materialize baseline was **763.774 ms p50**, with
763.470 ms core p50. The final result is **18.833 ms public / 18.562 ms core**,
about **40.6x** lower median latency on this workload. Final p95 is 30.078 ms,
showing run-to-run variation. No pre-implementation x86 speedup is claimed.
The WSL page-batching series moved A/tiled/full from 53.136 ms to 49.091 ms
p50; this smaller median difference is less decisive than the syscall-count
reduction below. No separate speedup is claimed for the revision-token change.

## Copy and memory accounting

The following are final macOS examples. MiB values describe controlled capacities,
not RSS; virtual reservation, supplied backing and logical samples are separate.

| Workload | Logical source / copied bytes | Output backing | Output virtual span | Modeled peak |
| --- | ---: | ---: | ---: | ---: |
| A / full | 268,435,456 / 268,435,456 | 256.000 MiB | 256 MiB | 523.020 MiB |
| A / one | 67,108,864 / 67,108,864 | 64.000 MiB | 256 MiB | 329.145 MiB |
| A / roi | 144 / 144 | 0.250 MiB | 256 MiB | 261.778 MiB |
| C / full | 201,326,592 / 268,435,456 | 256.000 MiB | 256 MiB | 525.267 MiB |
| view / full | 268,435,456 / 0 | 256.000 MiB | 256 MiB | 263.765 MiB |

All 4096² sources retain 256 MiB sample backing. A owns four image reservations;
B owns two; C and split/reassemble own one root. A materialization's final output
metadata is 5,768,498 bytes on macOS, with no full-image sample-staging buffer.
The modeled peak fell from 557,456,740 to 548,426,212 bytes, chiefly by publishing
all copied pieces in one prepared transaction. Sample backing itself is unchanged.
A common-owner view copies zero payload but retains the complete 256 MiB root,
including unrequested planes. Logical source bytes describe requested support,
not memory-bus traffic. C copies 256 MiB while its deduplicated source support is
192 MiB. WSL's smaller pages require more page-set metadata; its final A peak is
567,300,578 bytes. The CSV includes source/output metadata and reservation fields.

## Bottlenecks and admitted optimizations

1. **Repeated address/window validation.** Initial Xcode CPU Profiler had
   39,440 execution samples; read and write `row_run` were 16.379% and 18.999%
   self, with copy traversal 12.165%. The FMT-01 rectangle approach now validates
   bounded input/output rectangles once and copies contiguous row spans.
   Runs stop at ROI/tile boundaries and poll cancellation/currentness within
   1024 samples. Generic negative/zero strides retain their exact mapping.
2. **Repeated coverage publication.** A/B/C now prepare the complete requested
   output once, copy their exact mapped pieces and commit once. This avoids
   rebuilding prior output coverage/page sets per source. Source demand gaps
   are preserved, and no partial output escapes on failure.
3. **Per-page OS protection calls.** WSL strace for source setup plus two warmup
   and three measured A runs counted **393,224 mprotect calls before batching**
   and **392 afterward**. Only consecutive fresh pages are combined, in batches
   of at most 1024 pages; old pages/gaps terminate a batch. Failed or abandoned
   preparation withdraws only attempted fresh runs. Tests exercise two runs
   crossing the 1024-page limit, separated by existing pages, plus rollback,
   old-pixel preservation and cancellation. Syscall-failure injection was not run.
4. **Currentness probe overhead.** WSL perf showed `GraphSnapshot::current`
   at 21.59% whole-process self in one pre-change sample set. Snapshots now retain
   the small atomic revision token, avoiding weak-pointer reference-count
   operations in each probe. Graph replacement advances revision; context
   destruction still release-stores zero. This keeps the same stale semantics,
   with explicit replace/teardown tests and independent lifetime review.

The final macOS CPU-only trace has 16,366 execution samples. `_platform_memmove`
is 45.507% self and copy traversal 15.205%; remaining samples include coverage
maps and allocation. These are conditional cycle weights, not additive wall-time
percentages. No throughput or universal speedup guarantee follows.

## Xcode CPU Profiler and System Trace

```sh
python3 examples/channel_assembly_performance/profile.py \
  build/examples/channel_assembly_performance/photospider_channel_assembly_performance \
  build/fmt02-performance/new-cpu.trace

/usr/bin/arch -arm64 xcrun xctrace record --template 'System Trace' \
  --time-limit 8s --output build/fmt02-performance/new-system.trace \
  --launch -- "$PWD/build/examples/channel_assembly_performance/photospider_channel_assembly_performance" \
  4096 A tiled full materialize 1000 strict
```

Use fresh output paths. The CPU script uses the CPU instrument alone and exports
nonempty execution-stack cycle samples. The full **System Trace** template also
completed successfully on this host. Export `syscall`, `virtual-memory` and
`thread-state` tables using `xctrace export --xpath`, then run
`summarize_system_trace.py path/prefix` on `prefix-<schema>.xml`.

The system trace was captured after page batching and before the final revision-token
optimization. Execution-filtered records include 4,176 mprotect calls (4.356 ms
summed duration), 262 mmap calls, and 4,276,417 Zero Fill events (2,932.646 ms
summed duration). Zero Fill corresponds principally to fresh output backing.
Main-thread state records report 7,835.457 ms Running, 35.346 ms Interrupted,
49.535 ms Preempted and 657.099 ms Blocked across the recording, including setup,
verification and startup. These recorded intervals are not a decomposition of
the independent benchmark latency; no lock-contention bottleneck is inferred
from a sleeping worker or startup waiting. The remaining fresh-page zero fill
and memory copying are substantial costs under the selected fresh-output policy.

## WSL perf and strace

The tested checkout is `/home/alex/photospider-fmt02-20260924` in Ubuntu via
`ssh winpc`. Tools were unpacked into its `tools/runtime` without changing system
packages: strace 6.8, perf 6.8.12 and required shared libraries. The WSL kernel
permits user CPU-clock sampling. A reproducible command, with those tools on PATH:

```sh
perf record -e cpu-clock:u -c 20000000 --call-graph dwarf,4096 -m 256 \
  -o fmt02-perf.data -- ./build/examples/channel_assembly_performance/photospider_channel_assembly_performance \
  4096 A tiled full materialize 100 strict
perf report --stdio --no-children --sort symbol -i fmt02-perf.data
perf script -i fmt02-perf.data > perf.stacks
python3 examples/channel_assembly_performance/summarize_perf.py perf.stacks
strace -f -c -o strace-summary.txt \
  ./build/examples/channel_assembly_performance/photospider_channel_assembly_performance \
  4096 A tiled full materialize 3 strict
```

Fixed-period recordings before/after the final token change have 176/178 samples,
**zero reported lost samples**. The final stack filter selects 109 execution
samples; 99 have unresolved libc leaf symbols with an execution ancestor.
Their exact leaf function names remain unverified. Do not relabel unknown
addresses as memcpy or use these sparse samples for precise speedup claims.
Earlier frequency-mode attempts lost 86.84% and 99.75% of samples and are excluded.
The pre-batching fixed-period recording had 157 samples and zero reported loss.

strace reports system-call counts including setup and teardown; its durations
are heavily perturbed. The waiting worker's futex time overlaps active execution,
so it is not a measured CPU bottleneck. The mprotect count reduction is the
independently checkable page-batching result. Benchmark times come only from the
untraced serial matrix.

## Validation and artifacts

Both macOS and WSL pass the seven focused tests: channel assembly, channel
extraction, planar workflow, compiler, Value, resources and execution demand.
Both also build and execute the installed-package FMT-02 and FMT-01 consumers.
ClangFormat 21, cpplint and diff whitespace checks pass. Independent subagent
code/spec reviews closed their confirmed findings. No sanitizer, native Windows
or GPU validation is claimed.

Raw native data: `build/fmt02-performance/final-native/summary.csv`, per-case
logs and `final-native-cpu.trace`/XML/hotspot JSON. The initial scalar baseline is
`before-A.csv` and `before-A.trace`; `after-A` and `final-A` are intermediate
optimizations. System Trace is `final-system.trace`, exported schema XML and
`final-system.summary.json`. Final WSL data and perf outputs are copied under
`build/fmt02-performance/wsl/accepted-matrix/`; strace before/after is under
`wsl/performance/` and `wsl/final-matrix/`. Generated traces remain build artifacts.

Package 0.21.0 replaces tensor-description v1 with v2 and changes the private C++
snapshot-state representation. **Rebuild C++ consumers and re-encode old tensor
descriptions/overrides.** Operation C ABI and WorkflowDocument versions retain
their existing contracts. This work is a local implementation/validation delivery.
