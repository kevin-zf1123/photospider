# FMT-09 correctness-gated performance probe

Build the `photospider_transfer_performance` target. It uses the checked-in,
independent FMT-09 golden fixture, not a second invocation as its reference.
The fixture contains moderate values, exact branch neighbors and some protected
near-zero values. This is a reproducible diagnostic workload, not a model of
all photographs. Every requested output is checked after **every** timed call;
two calls warm the process before collecting the requested repetitions.
Verification and input construction are outside timed execute intervals.

```sh
cmake --build build --target photospider_transfer_performance -j4
B=build/examples/transfer_performance/photospider_transfer_performance
$B 256 srgb encode f32 strict tiled full 7 128 1 respect
$B 256 pq decode f64 x86_64 continuous r 7 128 1 respect
$B 258 hlg_oetf decode f64 strict tiled roi 7 256 1 respect
$B 256 acescc encode f32 apple_silicon tiled alpha 7 128 1 respect
```

Arguments, in order: size (1..4096), curve, direction (`encode|decode`), dtype
(`f32|f64`), profile (`strict|x86_64|apple_silicon`), storage
(`generic|tiled|continuous`), coverage (`full|r|alpha|roi`), repetitions (1..1000),
tile extent (128|256), CPU workers (1..64), metadata mode (`respect|raw`), corpus (`palette|sweep`), budget (`unmanaged|managed`).
A mismatched CPU profile fails explicitly; the tool does not silently substitute
another profile. `roi` is a 3x3 R-only rectangle crossing both tile boundaries
and requires size >= tile+2. Normal defaults are printed in `main.cpp`.

Curves: `linear`, `power_gamma` (gamma=2.2), `power_gamma2` (gamma=2), `srgb`,
`bt709`, `bt2020` (smooth), `bt2020_10`, `bt2020_12`, `bt1886` (Lb=.1, Lw=100),
`pq`, `hlg_oetf`, `acescc`, `acescct`. The unit test/oracle covers additional
parameters; this CLI deliberately uses a small reproducible parameter menu.
Both metadata modes transform RGB and copy alpha (a signaling NaN payload) exactly.

Compare two separately configured builds:

```sh
cmake -S . -B build-on -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release -DPHOTOSPIDER_TRANSFER_FAST_MATH=ON
cmake -S . -B build-off -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release -DPHOTOSPIDER_TRANSFER_FAST_MATH=OFF
cmake --build build-on --target test_transfer_math test_transfer_operations photospider_transfer_performance -j4
cmake --build build-off --target test_transfer_math test_transfer_operations photospider_transfer_performance -j4
ctest --test-dir build-on --output-on-failure -R '^test_transfer_(math|operations)$'
ctest --test-dir build-off --output-on-failure -R '^test_transfer_(math|operations)$'
```

OFF disables the interval/SLEEF filter only for FMT-09. It does not disable SIMD
in other NUM operators or the exact hardware gamma=2 specialization. Both builds
still require the repository's pinned SLEEF 3.9.0 source in `third_party/sleef`.
Do not apply global `-ffast-math` or relaxed FP contraction to either build.

CSV separates compile, pure preparation, first execute, warm execute p50/p95
and summed callback durations. Multiple workers can overlap callbacks, so
`wall - callback_sum` is NOT a scheduler overhead estimate. Use a one-worker run
and a sampled call graph to attribute overhead. Generic staged execution may
have two invocations (dependency request + publication) for one output.

`evaluated` counts selected sample attempts; `copied`/`views` are operator-side
bit-copy/view counts. `strict_math_calls` counts whole strict DAG attempts, not
individual pow/log instructions. `strict_fallbacks` counts fallback only for an
accelerated key; strict keys may have nonzero strict-math counts with zero
fallbacks. Exact anchors and the gamma=2 path do not enter the general DAG.

Planar sources publish only requested planes and regions. Generic external
bindings must be whole-dense, so their entire source is allocated. This is not
a fair comparison of equal source page states unless that difference is
accounted for. `source_read_bytes` is the kernel's existing counter; a zero for
a direct generic binding does NOT mean the callback read no input. Reserved
bytes are address span, backed bytes are storage pages, and `peak_live_bytes`
is the kernel's controlled-buffer accounting, not process RSS or total scratch.
Collect process memory and allocator samples separately.

Start with 4x4/256x256 and ROI/alpha cases, then increase to 4096x4096. General
Float64 strict transcendental workloads can spend most time in bounded
multiprecision fallback; do not infer their throughput from Float32 runs.
The benchmark explicitly raises both public dependency-work limits to allow
these workloads; applications should choose finite budgets and cancellation
policies appropriate to their execution context.


## Revision-2 comparisons

`palette` preserves the original moderate golden selection. `sweep` selects
128 checked-in, fixed-seed uniform/log-distributed inputs per curve/direction/
dtype from the independent 6,656-case fixture. Pixels still repeat this finite
corpus: this is not a nonrepeating image or a random parameter benchmark.
Regenerate/check it with `python tests/oracles/fmt09_sweep.py --check` (mpmath is
needed only for oracle development). Each expected bit pattern agrees at
180 and 360 decimal digits; the C++ kernel never supplies the reference.
Log-distributed inputs also use directly rounded high-precision powers at both
precisions, so the fixture does not depend on host libm `pow` rounding.

The last CLI argument `managed` enables a real root ResourceBudget. The output
reports the exact issued work delta per execution. Do not use the unfixed v1
planar root's zero-work result as an equivalent managed-performance control.

Three independent build flags default to ON:

- `PHOTOSPIDER_TRANSFER_FAST_MATH`: existing certified SLEEF interval filter.
- `PHOTOSPIDER_TRANSFER_COMPACT_MATH`: 8-word strict enclosure tier; capacity or
  unresolved rounding retries the retained 192-word evaluator, not an estimate.
- `PHOTOSPIDER_TRANSFER_GAMMA2_SIMD`: signed square/sqrt and IEEE bit-classification vector kernels. OFF retains
  the allocation-free scalar span specialization and exact hardware arithmetic.

For a wide/scalar algorithm control, keep FAST_MATH=ON and set the other two
flags OFF. An all-OFF build is a separate final fallback test. Disabling the
compact tier does not undo the required currentness or resource-accounting fixes.
The implementation/cache identity includes all three settings.

Use two separate builds and run serial ABBA processes; each process does two
warmups followed by the requested repetitions. All samples pass their own output
gate *outside* the timed interval before a `SAMPLE` line is written to stderr.
The CSV p50 and p95 now use linearly interpolated quantiles; the comparison
script pools both processes' measured samples and uses `statistics.median`.
The historical floor-index p95 must not be compared as the same statistic.

```sh
python examples/transfer_performance/compare.py \
  --before build-wide/examples/transfer_performance/photospider_transfer_performance \
  --after build-on/examples/transfer_performance/photospider_transfer_performance \
  --output measurements --repeats 5 --profile x86_64 --cpu 0
```

On macOS use `--profile apple_silicon` and omit `--cpu`. On FreeBSD `--cpu 0`
uses `cpuset -l 0`; the chosen CPU should be characterized on hybrid systems.
`--managed` requires **both** fixed kernels. `--case` restricts the matrix;
`--include-4k` adds a large gamma=2 run with every iteration fully checked.
An existing output directory is rejected. No compiler/Ninja/CMake process may
be active at each measurement entry. This check is not a system-wide lock;
exclude builds/tests, frequency changes and unrelated work externally as well.

To compare against the original kernel, compile this revision's benchmark
`main.cpp` with original public headers and the original library, but include
this revision's `tests/` for both fixtures. The original CLI cannot be used
unchanged: it only checked its first execution and has no corpus/work columns.
The supplied results record exact commands, raw stdout/stderr, process exits,
per-execution samples and the environment; no baseline performance is inferred
from another OS or machine.

## Hotspot optimization measurements

`fmt09_sweep.py --samples 512 --table out/oracle.txt` generates a larger external
180/360-digit oracle table. Pass `file:out/oracle.txt` as the corpus argument:
RGB uses consecutive distinct entries and rejects tables shorter than the image.
Alpha remains a repeated bypass payload. Input construction and table parsing
are outside execute timing. `compare.py --all-curves --corpus-table out/oracle.txt`
compares all 13 supported parameter selections, both directions and dtypes,
and strict/native profiles. Add `--managed` for root accounting and `--workers`
for worker scaling; use a workload with multiple tiles for actual parallelism.

The current compact tier has 512-bit storage with the same certified endpoint
agreement and wide fallback. Per-callback rational scratch and constants are
reused; long division skips a prefix that cannot contribute quotient bits.
Planar checkpoints borrow the host status and pure-work admissions use a bounded
atomic CAS. Mixed work/I/O/stage admissions remain one transaction. No deferred
accounting or speculative work credit is used.

General interval filtering gathers up to 16 same-branch lanes. Strict lanes
retain independent exact refinement and share per-callback constants/scratch.
The small tier divides normalized base-2^64 words with exact trial-quotient
correction; the wide tier retains bitwise division. Rational fallback removes
common binary factors, reuses remainder storage, and handles single-word
denominators directly. The independent Python `divmod` fixtures are reproduced
with `python3 tests/oracles/fmt09_division.py --check`.
