# Performance Review Follow-up

This review follows the recommendations in the supplied September 28 performance
package. The package is advisory evidence: its findings describe hypotheses and
candidate work, and do not establish that every proposal is valid or has been
implemented. The current code and the paired measurements below determine status.

## Measurement scope

The post-review paired runs were collected on Apple M5 with 10 CPU cores and
32 GiB, macOS 27.2 build 26B5091g. The kernel used RelWithDebInfo and Apple Clang
21; PixelOE used Release, precise floating-point options and Slang 2026.18.2.
The benchmark JSON records five per-process measured-call medians per arm. Tables
report the median of those five medians and show their minimum to maximum spread.
Build, setup and reference comparison are outside the measured call where defined
by the corresponding runner. The runs establish only these workloads on this
machine; they do not predict other CPUs or GPU vendors.

## Validated changes

### Gaussian exact arithmetic

The CPU kernel multiplies the binary significands of the two baked coefficients
and the input sample in a compact 159-bit product before placing that result in
the existing 68-limb exact accumulator. This avoids repeated multiplication of
large fixed-width values with long zero regions. Each invocation-local arithmetic
slot also computes the exact normalization denominator once for its immutable
coefficient table and reuses it when resetting the sample numerator. Slots remain
independent, and separate invocations own separate coefficient and arithmetic
state.

| Change | Workload | Before, ms | After, ms | Paired notes |
| --- | --- | ---: | ---: | --- |
| Compact tap product | Whole, 32 × 32, 1 worker | 30.758 (30.342–31.660) | 7.314 (7.266–7.484) | Peak managed bytes decrease by 34,816; declared work decreases by 68. |
| Compact tap product | Whole, 64 × 64, 4 workers | 30.875 (30.742–31.134) | 7.297 (7.202–7.385) | Peak managed bytes decrease by 34,816; declared work decreases by 272. |
| Compact tap product | Tiled, 32 × 32, 4 workers | 20.821 (20.581–21.677) | 12.057 (11.909–12.252) | After-change managed peak ranges from 2,280,512 to 2,427,800 bytes. |
| Reused normalizer | Whole, 32 × 32, 1 worker | 7.365 (7.302–7.507) | 6.181 (6.133–6.246) | Peak managed bytes increase by 512. |
| Reused normalizer | Whole, 64 × 64, 4 workers | 7.501 (7.316–7.574) | 6.311 (6.179–6.439) | Peak managed bytes increase by 512. |
| Reused normalizer | Tiled, 32 × 32, 4 workers | 12.161 (11.819–14.218) | 11.583 (11.428–12.162) | Timing ranges overlap. |

All paired rows report output bit equality. The compact product shows a large
reduction in these Whole and tiled cases. Denominator reuse shows a smaller Whole
gain, while tiled timing variation limits the conclusion for that mode. Raw rows
are in `out/performance-review/gaussian-compact-paired.json` and
`gaussian-normalizer-paired.json`.

The recorded FreeBSD Intel UHD 770 Vulkan evidence covers 94 independent
workflows and 707 output-bit comparisons. The newer 256-lane native differential
tests add 527 output comparisons across two dtypes and cancellation. The exact
CPU oracle and native tests protect the rounding, special-value and cancellation
contracts. This finite evidence does not establish NVIDIA or Linux Vulkan
performance.

The compact-product change also shifted the sampled Gaussian arithmetic profile.
Before the change, `multiply_fixed<68>` led with 4,733 of 7,806 samples (60.6%
of attributed samples). Afterward, wide multiplication was absent from the top
12; `FixedInteger<68>::subtract` had 2,512 of 7,966 samples (31.5%), `add` had
797 (10.0%), and `compare_keys` had 685 (8.6%). Exact subtraction, addition,
comparison, shifting and movement account for the remaining sampled arithmetic
costs. Exact ratio rounding is the next profiling area; no follow-on optimization
is included here. These samples do not measure completed operations per second.
A separate fixed eight-second capture of each build ended when the profiler's
time limit killed the command; those sample counts are not a throughput comparison.
The summaries
are in `out/performance-review/gaussian-profile-summary.json`.

### Perlin CPU interpolation

The exact CPU evaluator now applies seven nested integer lerps to the eight corner
values rather than expanding eight weighted terms. Each lerp appends 5*q
denominator bits: corner values have denominator D, the four x lerps D^6, the two
y lerps D^11, and the final z lerp D^16. These are exact signed integer
numerators at each level, and output receives one final IEEE rounding. The shader
continues to evaluate the expanded polynomial; the CPU result does not imply a
shader change.

| Mode | Workers | Expanded form, ms | Nested lerps, ms | Paired notes |
| --- | ---: | ---: | ---: | --- |
| Whole, 16,384 samples | 1 | 23.607 (22.860–24.714) | 13.575 (13.249–13.964) | Declared work decreases from 63,045,632 to 29,540,352. |
| Whole, 16,384 samples | 4 | 8.360 (8.143–8.994) | 5.927 (5.704–6.312) | Modeled peak remains 3,196,624 bytes. |
| Tiled, 16,384 samples | 4 | 18.446 (14.206–19.547) | 15.881 (14.900–16.641) | Timing spread is larger and overlaps. |

The paired rows are bitwise equal to the Whole reference. Arithmetic, Whole and
tiled independent-oracle comparisons each cover 1,566 outputs. These timing gains
support the CPU expression change for this workload, not general CPU or GPU
performance. Raw rows are in `out/performance-review/perlin-lerp-paired.json`.

### Gaussian Metal dispatch capacity

The GPU executor now dispatches up to 256 output lanes on Metal and 64 on Vulkan.
Each dispatch handles up to 16 taps per lane, and one synchronous submission may
group two ordered dispatches. Static workspace admission reserves 1,400,832 bytes
for 256 lanes on both backends; Vulkan uses 64 lanes per dispatch but retains this
conservative admission. Coefficient tables, output and other owners add to total managed workspace. The
Metal paired runs show fewer dispatches and submissions, with greater reported
managed peak:

| Input | Prior dispatch/submission count | Current count | Prior, ms | Current, ms | Peak managed bytes, prior → current |
| --- | ---: | ---: | ---: | ---: | ---: |
| 32 × 32 | 32 / 16 | 8 / 4 | 45.835 (45.711–45.857) | 11.799 (11.774–11.923) | 393,840 → 1,442,416 |
| 128 × 128 | 512 / 256 | 128 / 64 | 733.153 (729.338–769.127) | 185.632 (184.896–185.975) | 639,600 → 1,688,176 |

The benchmark records output bit equality. The reduced host submission overhead
and increased workspace are measured on this M5 Metal configuration. The results
do not transfer to Vulkan, where dispatches remain 64 lanes, or to other devices.
Raw rows are in `out/performance-review/gaussian-gpu-paired.json`.

### Combined Gaussian CPU comparison

An additional five-pair comparison measured the original CPU implementation
against the combined current implementation after the compact product and
per-slot normalizer changes. Each process executed 17 measured calls, and the
table reports the median of the five process medians with their spread. All rows
reported bitwise equality to the Whole reference.

| Mode and workload | Original, ms | Combined, ms | Peak managed bytes, original → combined |
| --- | ---: | ---: | ---: |
| Whole, 32 × 32, 1 worker | 31.661 (30.596–32.478) | 6.129 (6.127–6.587) | 554,496 → 520,192 |
| Whole, 64 × 64, 4 workers | 31.129 (30.775–36.206) | 6.146 (6.050–7.034) | 579,072 → 544,768 |
| Tiled, 32 × 32, 4 workers | 21.028 (20.633–22.071) | 12.097 (11.437–12.355) | 2,408,584–2,484,976 → 2,342,296–2,412,872 |

The medians are approximately 5.17×, 5.06× and 1.74× lower, respectively.
The tiled peak varied by invocation; its reported range is shown rather than a
single byte count. This end-to-end comparison combines the two CPU arithmetic
changes and does not measure the GPU dispatch change. Raw pairs are in
`out/performance-review/gaussian-final-paired.json`.

## Recommendation disposition

Six bounded changes have direct implementation and correctness evidence: the
compact Gaussian tap product, reuse of the exact Gaussian normalizer in each
invocation-local slot, CPU Perlin nested lerps, wider Gaussian Metal dispatches,
the PixelOE SIMD blur tail and CPU Whole quantization label buckets. Normalizer
reuse is partial because coefficients and normalization are not shared across
tiled sessions. Label bucketing is partial because it reduces repeated scans but
retains the large partial table. Performance gains are workload-specific; the
paired measurements above and below state where they are supported.

The status applies to each recommendation from the package, not to every related
mechanism already present in the repository. “Implemented” means that this
follow-up includes the behavior and direct evidence listed above. “Partial” means
the measured or implemented scope covers only part of the recommendation, or a
candidate still awaits its performance decision. “Deferred” includes proposals
without sufficient evidence and experiments that were rejected.

| ID | Status | Current disposition |
| --- | --- | --- |
| S01 | Deferred | A CPU range-grain experiment was run and reverted. Its 128 × 128 one-worker case had a lower candidate median but a wide spread; the four-worker 128 × 128 case was flat, and the 512 × 512 four-worker case regressed from 32.001 to 35.672 ms. No stable gain was demonstrated. |
| S02 | Deferred | Event-driven cancellation wakeups need lost-wakeup, cancellation-latency and contention evidence before changing stage lifetime behavior. |
| S03 | Deferred | Short-graph dispatch and completion overhead has not been isolated with public end-to-end measurements. |
| S04 | Deferred | Reusable prepared handles require registry and plugin-lifetime analysis; no execution-handle change was made. |
| S05 | Deferred | Resource-credit contention was not shown to be the relevant bottleneck in these follow-up workloads. |
| S06 | Deferred | Floating-environment guard reduction needs direct short-task measurements and full exception-state checks. |
| S07 | Deferred | The worker-count policy remains unchanged. No controlled measurement covers CPU affinity or cgroup quotas together with total client concurrency, so quota-aware worker sizing is unvalidated. |
| V01 | Deferred | The recommendation's shared queue premise is not established for the current per-context device ownership and single GPU-pool worker configuration. Queue-wait contention was not measured. |
| V02 | Deferred | Persistent Vulkan submission slots and constant-buffer pooling were not measured on the available M5 Metal host. |
| V03 | Deferred | Native allocation pooling and residency policies require backend-specific profiling and lifetime validation. |
| V04 | Deferred | Pipeline validation and cache changes have no measured cold-start or steady-state result. |
| P01 | Implemented | The CPU exact Perlin evaluator uses seven nested lerps; paired Whole timing and 1,566-case CPU, Whole and tiled oracle comparisons support the change. The shader retains its prior polynomial. |
| P02 | Deferred | Block decoding and a contiguous-layout fast path were not implemented or measured. |
| P03 | Deferred | Workspace remains sized for the current maximum slot configuration; quota-sensitive sizing has not been validated. |
| P04 | Deferred | The tiled traversal and fragment-query proposal has no new paired measurement or implementation. |
| P05 | Deferred | GPU precision-tier isolation remains unimplemented. No high-q outlier A/B was run, so the proposed grouping change has no performance result. |
| G01 | Partial | Each invocation-local arithmetic slot prepares and reuses its exact normalizer. Sharing coefficient and normalization preparation across tiled sessions remains unimplemented; Whole gains are stable in these pairs, while tiled timing ranges overlap. |
| G02 | Implemented | Compact 159-bit tap products replace repeated wide sparse multiplication. Paired Whole and tiled cases report bitwise reference equality and lower medians. |
| G03 | Deferred | Moving boundary mapping and authorized reads out of the tap loop requires a separate exact-support audit and workload comparison. |
| G04 | Deferred | Tiled support and halo reuse remains unimplemented; no coefficient or support-cache evidence was added. |
| G05 | Implemented | Metal dispatch capacity is 256 lanes while Vulkan remains 64; paired Metal runs reduce dispatches by 4× and report faster public calls with higher managed peak. |
| G06 | Deferred | Exact separable convolution remains an architectural proposal; two-pass rounded convolution would violate the current one-final-rounding contract. |
| X01 | Deferred | PixelOE GPU stage batching was not isolated and measured in this follow-up. |
| X02 | Deferred | CPU ping-pong buffer reuse was tried and reverted. Five-pair public A/B runs showed no stable latency or peak-memory gain: CPU 1-worker 512² changed 74.849→75.236 ms, CPU 4-worker 1024² 122.162→126.687 ms, and tiled 4-worker 512² 36.381→36.694 ms, with unchanged peak bytes. |
| X03 | Deferred | A fused CPU k-centroid path needs an isolated end-to-end measurement and numerical comparison. |
| X04 | Implemented | The one-vector SIMD tail passed independent review and 4,030 bitwise SIMD/AOT comparisons, plus CPU, tiled and stage checks. Five-pair timings show gains at 24 × 256 (3.381 → 3.137 ms) and 128 × 128 (6.049 → 5.886 ms), while 256 × 256 is indistinguishable. |
| X05 | Deferred | Input/output validation fusion and zero-copy eligibility were not implemented; ownership and output-publication paths remain to be measured. |
| X06 | Deferred | Staged tiled execution still uses full-image intermediate buffers; no bounded region-pipeline change was made. |
| X07 | Deferred | Generated-kernel string containers and default stage timing were not separately profiled or changed. |
| X08 | Partial | The CPU Whole path with its SIMD-enabled dispatcher accumulates per-chunk label buckets while retaining the existing K × 4 Int64 table, sample order within each chunk and `km_update` sequence. Bucket accumulation remains scalar. Five alternating pairs show lower medians on the heavy quantization case and small, workload-sensitive changes on default quantization. The partial-table memory reduction is not implemented. The scalar, tiled and generated GPU paths are unchanged. |

### PixelOE CPU Whole follow-up

The one-vector blur-tail change passed independent review. Its five-pair medians
were 3.381 → 3.137 ms for 24 × 256 and 6.049 → 5.886 ms for 128 × 128, with
unchanged checksums and peak bytes. At 256 × 256, medians were 19.694 → 19.601 ms
with overlapping ranges, so this case does not establish a gain. The detailed
measurements and maintained reproduction commands are in
[`PixelOE performance`](../../plugins/ops/PixelOE/PERFORMANCE.md).

The CPU Whole quantization change targets a heavy public workflow: 256 × 256,
`pixel_size=2`, `num_colors=128`, quantization enabled, color matching disabled,
`thickness=0` and no post-upscale. Each arm used five alternating pairs with seven
measured executions per process. Median execution time fell from 79.242 to
63.774 ms with one worker, and from 22.365 to 18.754 ms with four workers. The
four-worker bucket arm ranged from 18.669 to 24.350 ms, including one high sample.
Exact output bytes, checksums and the 3,514,302-byte peak matched between arms.

Default quantization at 512 × 512 changed from 79.724 to 78.585 ms with one
worker and from 25.997 to 25.353 ms with four workers. Repeat-k-means medians
changed from 26.188 to 26.071 ms. These small differences do not establish a
general quantization speedup. Exact output bytes matched in all paired runs.
The label-bucket implementation retains the existing `int64[K*4]` partial table;
it reduces repeated scans over pixels within each chunk, but does not reduce that
table's memory. Its scope is the CPU Whole SIMD path and full-grid K-means. Scalar,
CPU tiled, generated-kernel and GPU paths retain their prior behavior.

The SIMD check now covers 4,102 bitwise SIMD/AOT cases, including 48 full-grid
and 24 partial-grid label-bucket cases. Independent fixed-point stage witnesses
and CPU/tiled contracts pass. A native FreeBSD AVX2 check also passes all 4,102
cases; it is arithmetic/stage correctness evidence, not a public-workflow
performance comparison.

## Native arithmetic-only measurements

A separate FreeBSD 15.1 amd64 run used Clang 22 and the same isolated arithmetic
harness for both versions, pinned to CPU 0. Three rounds ran in fixed order; the
arms were not alternated. The benchmark linked against a compatible existing
kernel static library and compares arithmetic changes only. Matching checksums
provide a consistency check, but this harness did not compare all output bytes.
Separate oracle and unit tests validate the exact arithmetic.

| Kernel case | Samples | Earlier headers, ms | Current headers, ms | Median reduction |
| --- | ---: | ---: | ---: | --- |
| Gaussian | 10,000 outputs | 161.152 (160.485–169.136) | 46.251 (45.911–48.036) | 3.48× faster |
| Perlin full precision, q=63 | 1,000 | 16.271 (16.253–16.297) | 15.453 (15.420–15.546) | 5.3% |
| Perlin full precision, q=1074 | 100 | 15.276 (15.247–15.660) | 4.985 (4.969–5.010) | 3.06× faster |

These limited fixed-order samples exercise the exact arithmetic kernels, not
public workflows, scheduling, validation, allocation or end-to-end publication.
The arithmetic harness and raw JSONL are in ignored `out/performance-review/`
and are not a maintained reproduction tool. The tracked correctness entry points
remain available with:

```sh
cmake --build build/kernel-dev --target test_gaussian_exact test_perlin_exact -j 8
ctest --test-dir build/kernel-dev -R '^test_(gaussian_exact|perlin_exact)$' --output-on-failure
```

These commands verify arithmetic behavior; they do not reproduce these timings.

## Files and evidence limits

The Gaussian and Perlin implementation pages record current contracts and their
paired arithmetic results. [PixelOE performance](../../plugins/ops/PixelOE/PERFORMANCE.md)
now records the reviewed SIMD-tail timing and current-build reproduction commands.
CPU ping-pong reuse remains omitted from that implementation page because its
candidate did not establish a gain.

The review package's original environment log describes the initial audit host;
it does not describe the later paired results. The later measurements are
identified above from the benchmark follow-up records. No claim is made for
unmeasured CPU models, Linux or NVIDIA GPU execution, Vulkan submission pooling,
concurrent multi-client workloads or the broader option space.
