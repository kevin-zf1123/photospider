# FMT-04 / FMT-05 revision 2 native review

Reviewed on 2026-09-26 in `ops-impl-FMT04-05`, based on HEAD
`ebc281137d53153228de0087169b81b345ef96d8` plus the existing FMT-04/05
implementation and `FMT04_FMT05_revision2.zip`. The 17 revision replacements
matched the bundle's declared input versions. All changes remain local.

## Correctness fixes found during this review

- Exact-planar callback submission now precharges cumulative stages. A zero
  stage allowance rejects before callback entry; a one-stage allowance permits
  one execution and rejects the next execution on the same context. The charge
  is inside the validation/materialization loop.
- A callback's detected Protocol failure survives subsequent scratch failure,
  cancellation and plan invalidation in both the registry and executor gates.
- Inserting a new alpha while retaining a shared old alpha now creates fresh
  normalized coverage metadata. The retained slot keeps its old name and local
  or inherited encoding. Moving/replacing a private alpha still preserves its
  applicable metadata.
- Formatted the affected C++ with ClangFormat 21.1.3, supplied direct standard
  headers, and corrected lint issues. No floating-point flags or numerical
  profile defaults were relaxed.

Each behavioral defect failed a new direct regression before its fix. Independent
read-only standards/safety and specification reviews found no remaining required
issue in their scoped re-review. The exact-contract regression covers stage limits
0/1 in Auto/Materialize and Protocol with cancellation/graph invalidation. It does
not force the multiple-owner Auto retry or simultaneous Protocol/scratch-failure
combination; those control-flow paths received static review.

## Native validation

Both machines passed all 21 targeted CTests from the bundle's
`tools/focused-targets.json`, including alpha math, integration, exact contract,
numeric conversion, metadata, channel editing/assembly/extraction, planar input,
Region, dependency and ordered execution tests. Final totals were 21/21 on each
machine. The independent Python golden fixture check passed: 2,048 input pairs,
4,096 expected results. Native scalar and NEON/AVX2 paths were actually executed.

A separately configured consumer of the installed package built and ran on each
machine. Six additional consumer commands exercised associate, unassociate, set
(view), extract, remove and opaque at a 130-square image's boundary-crossing ROI.
ClangFormat 21.1.3, cpplint 2.0.2 and `git diff --check` passed. This was focused
validation, not a full release, sanitizer, TSan or exhaustive floating-state gate.

## Measurement contract

| Item | macOS | FreeBSD |
| --- | --- | --- |
| CPU | Apple M5 | Intel Core i9-12900 |
| Compiler | Apple Clang 21.0.0 | Clang 22 with the existing separate libc++ toolchain |
| Build | RelWithDebInfo, `-O2 -g -DNDEBUG`, existing strict FP options | Same optimization/FP policy |
| ISA | Native AArch64 NEON | Runtime-detected AVX2 |
| Affinity | Not pinned | `cpuset -l 0` |
| Power observation | `pmset` reported no recorded thermal/performance warning | `machdep.hwpstate_pkg_ctrl=0`; no tuning changes made |

FreeBSD's system libc++ lacks floating `from_chars` needed by the existing
expression parser. The initial build exposed this; the successful build uses the
already available Clang 22/libc++ wrapper. This does not claim system-libc++ support.

The **before** comparison retains all final shared correctness fixes and substitutes
only the bundle's `comparison/fix_only/alpha_math.hpp` and `alpha_operations.cpp`.
The comparison builder compiles that translation unit with the same generated
compile command, substitutes its object in a copy of the final static archive,
and relinks the same driver/test objects. All other objects are identical. Both
comparison alpha integration and exact-contract executables passed. Consequently
this comparison measures numeric-loop and row-access optimization, not the cost of
restoring budget enforcement. The two effects are not separately isolated.

`compare.py` ran 38 configurations in ABBA order (152 validated processes) with two
warmups and 11 measured executions per process. A separate latency suite ran 11
configurations (44 processes), each with 101 measured executions. The table is the
median of the two process p50 values for each variant, not a pooled percentile or
confidence interval. Compilation, input setup and first-run sample verification
are outside the reported hot execution interval. Callback times are also recorded.
The lock and compiler-activity check prevent cooperating measurements/builds from
overlapping; they do not eliminate arbitrary background load or clock variation.

## Results

Full output, 512×512, tiled 128×128, mixed alpha 0/0.5/1, one worker,
strict profile, explicitly selected SIMD, unmanaged counters. Times are ms.

| Operation | Type | M5 before → after | Speedup | i9 before → after | Speedup |
| --- | --- | ---: | ---: | ---: | ---: |
| Associate | f32 | 10.362 → 1.122 | 9.24× | 17.301 → 1.822 | 9.49× |
| Unassociate | f32 | 11.161 → 1.136 | 9.83× | 17.666 → 1.881 | 9.39× |
| Associate | f64 | 9.260 → 1.629 | 5.69× | 10.001 → 2.610 | 3.83× |
| Unassociate | f64 | 9.004 → 1.617 | 5.57× | 10.361 → 2.753 | 3.76× |

The scalar path also improved: 5.08–7.02× on M5 and 2.92–6.81× on i9 for these
four configurations. Strict `auto` still selects scalar. Explicit SIMD gains
therefore do not describe every default invocation.

For continuous 2048-square f32 SIMD images, associate/unassociate respectively
measured 150.213→8.278 / 167.342→9.146 ms on M5 and
273.805→20.346 / 279.952→20.924 ms on i9. Continuous storage and larger workloads
must not be directly equated with the 512-square tiled case.

SetAlpha identity-view measured 4.360→0.874 ms on M5 and 9.424→1.565 ms on i9;
materialization measured 4.782→1.236 and 10.191→2.283 ms. Its view path still
validates the requested samples. Extract and opaque were approximately unchanged.
A remove-helper improvement on M5 cannot be attributed to the changed alpha
arithmetic; that lowering path does not use it.

Small ROI results do not establish a universal speedup. In the 101-run suite,
M5 continuous associate moved from 160.626 to 166.812 µs (3.9% slower); the other
five ROI configurations ranged from 0.8% to 2.4% faster. On i9 the six ROI cases
were within 0.0–1.8% slower. The shorter main suite had larger swings, so small
ratios should be treated as host-sensitive observations, not statistically
established algorithm regressions. No profile default was changed from these data.

Managed-on cases succeeded, and cumulative issued work matched before/after in
all 38 configurations on both machines. For one representative after f32
associate process, M5 total/callback p50 was 1099/959 µs and i9 1821/1627 µs.
Each read and materialized 4,194,304 bytes. Set view copied zero result bytes,
whereas materialize copied 4,194,304 bytes. Opaque read zero source bytes.
Managed host peak for f32 associate was 9,256,954 bytes on M5 and 9,662,362 bytes
on FreeBSD; it follows capacity accounting and is not RSS. Full raw data includes
metadata/referenced peaks, p95, first-run and compile times.

## Profiling interpretation

macOS `sample` collected three-second snapshots separately from timing. Before,
leaf samples concentrated in `memmove` (253), `math_span` (98) and memcpy stubs
(57). After, prominent leaves included `finite_checked` (115), `planar_read` (46),
`memmove` (36) and allocation routines. Waiting-thread samples are present and
must not be interpreted as CPU work. The profiler used enough repetitions to
keep each binary alive; different repetition counts are not a latency comparison.

FreeBSD `pmcstat` instruction sampling for 300 executions attributed 74.72% of
before samples to `math_span`, with 7.17% in memcpy. After, the prominent leaves
were memset (17.78%), read `row_run` (11.52%), `vector_checked` (10.64%),
`planar_read` (5.52%) and allocation/free. These are instruction-sample shares,
not elapsed CPU-time shares. Whole-process profiles include setup and the
first verification, which have a larger relative share in the faster version.

A separate two-counter run observed 49,674,001,928→6,802,807,602 instructions and
24,803,543,460→2,949,390,805 unhalted cycles. The before counter output has an
intermediate and final interval; both are summed. Counter runs are separate from
uninstrumented latency and do not justify reading IPC as a universal property.
Unknown-function samples were 3312/45200 before and 165/2972 after; the after
export also reported three dubious frames. Those symbolization limits remain.

The data support the removal of repeated per-sample handling and copying. Row
lookup, initialization and allocation are now proportionally more significant.
Additional optimization would need its own workload-specific measurement; these
results do not prove scheduler scaling or multithreaded operator speedup.

## Reproduction and local artifacts

The local review artifacts are under `out/revision2-review/` in this worktree:

- `focused-tests.log`, `fixed-tests.log`, `style-final.log`, `consumer-workflows.log`;
- `stage-repro.log` and `repro2.log` contain pre-fix failures;
- `build_comparison.py` plus `mac-before/build-commands.log` record the exact
  comparison-object and relink procedure;
- `mac-abba/` and `mac-latency/` contain summary/raw CSV, every command and hot
  sample, and environment/binary identity records;
- `mac-profile/` contains the sample snapshots;
- `freebsd-results/` contains native build/test/consumer logs; its nested
  `out/revision2-review/freebsd-{abba,latency,profile}/` contains remote samples,
  counter output, binary identities and exported call graphs;
- `mac-environment.txt` and `freebsd-environment.txt` record native observations.

The FreeBSD source/build remains at `/tmp/photospider-fmt0405-revision2`.
The final local build is `build/fmt04-05-review`. The comparison used:

```sh
python3 out/revision2-review/build_comparison.py \
  build/fmt04-05-review \
  out/revision2-review/FMT04_FMT05_revision2/comparison/fix_only \
  out/revision2-review/mac-before
python3 examples/alpha_performance/compare.py \
  --before out/revision2-review/mac-before/photospider_alpha_performance \
  --after build/fmt04-05-review/examples/alpha_performance/photospider_alpha_performance \
  --output out/revision2-review/new-comparison --suite extended --repetitions 11
```

Use the remote paths, Python 3.12 and `--launcher 'cpuset -l 0'` on FreeBSD.
Repeat with `--suite latency --repetitions 101` in another output directory.
