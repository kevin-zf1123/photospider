# NUM-13 Whole execution

All six formal prefix_sum/integral_image keys use Whole. Nonempty demand reads
and validates complete input, computes complete output including padding, then
projects. Empty reads nothing; a zero-boundary-only projection still reads input.
Any input edit invalidates all recorded observations. Integer overflow at any
complete output coordinate fails Domain/Run; for[INT64_MAX,1,-1], k=2 now fails
even a k=3-only projection. Floating overflow remains a numerical result, without
contaminating exact carry. Public keys/shapes/dtypes and mathematical prefix rules
remain unchanged. Legacy unsuffixed keys are separate.

Prefix keeps an exact accumulator and a conversion snapshot. Integral visits
planes independently, lower-numbered selected axis as outer rows and the other
as inner columns. Current exact row prefix plus saved previous-row rectangle
covers the desired rectangle with disjoint contributions. A compact column
carry saves magnitude/sign, first NaN, both infinity flags and all-negative-zero
before destructive conversion. Prior rows retain NaN priority over the current
row; every plane resets all columns. No rounded recurrence or repeated rectangle
scan remains. Numerical profiles retain their own contract; no NUM-14 certificate
is reused and Scalar/NEON/AVX2 plus NUM-14 Accelerate/SME comparisons remain.

Fixed state is5904 payload bytes; integral column elements add560*W metadata
bytes plus allocator header/alignment/Entries on this arm64 build. W is the
higher-numbered selected axis extent. Complete input/output payloads are additional.
Coordinate vectors are bounded by rank8. Exact work/cancellation checks cover
reads, resets, merges, conversion and publication; failed output/state/columns
are released. There are no per-output point plans or dependency association rows.

## Public workflow and independent validation

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric --target photospider_numeric_scans -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_scans strict
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_scans apple
python3 examples/numeric_workflow/scan_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_scans strict
python3 examples/numeric_workflow/scan_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_scans apple
```

Both public suites and2544 independent Fraction/raw-bit cases per profile pass.
The oracle directly enumerates each source rectangle/prefix and checks the entire
integer output domain for Whole overflow. Added5x7 and3x2x4 cases cover exact
cancellation, extreme values, cross-plane reset and source NaN order. Public
prefix[1,2,3] ->[0,1,3,6] and integral[[1,2],[3,4]] ->
[[0,0,0],[0,1,3],[0,4,10]] pass, alongside floating overflow recovery, generated
NaN versus later source NaN, all-negative-zero and four fenv modes/flags.

Tests cover negative/zero/unaligned layouts, full typed rejection including zero
boundary demand, Empty/pre-cancel, output-shape cap, unrequested integer overflow,
full-input/dirty behavior, work/full-output/scratch budgets, exact-column metadata
capacity and active cancellation with cleanup. A4096-term sparse projection now
owns full output under131072-byte payload capacity. Five focused CTests,
ClangFormat21/cpplint and independent exact-algorithm review pass. No x86 runtime
validation here; older regional WSL/installed tests are historical.

## Public latency and exact numerical core

Apple M5/macOS27.0 (26A5425a), Clang21.1.3 O2/RelWithDebInfo,
-fno-fast-math -ffp-contract=off, package0.18/traits16. Before adapter fromb7466215
links to the same current kernel. One worker, result/dependency caches off,
1 GiB payload,2^40 dependency work,512 MiB dependency state. One warmup and
seven measured calls; compile/freeze and complete analytic verification are
outside timing. Float64 inputs are all ones: prefix[N] outputs i at boundary i;
integral[sqrt(N),sqrt(N)] outputs y*x at each padded coordinate.

Core is the actual callback with prebuilt inputs/prepared metadata, including
state/column/output allocation, exact carry and final rounding, excluding public
scheduling/collection and managed work-ledger overhead. Milliseconds median[min,max]:

| Input N / operation | Before public | Whole public | Apple callback core | Scalar core median |
| --- | --- | --- | --- | --- |
|256 / prefix|2.7405[2.6242,2.8247]|.0938[.0893,.1149]|.0515[.0495,.0566]|.0515|
|256 / integral|79.3977[78.8160,80.4124]|.1215[.1093,.1540]|.0621[.0613,.0644]|.0660|
|16384 / prefix|failed|3.9634[3.9100,4.3085]|3.3019[3.2425,3.3439]|3.3413|
|16384 / integral|failed|4.7248[4.6471,4.8318]|4.1420[4.0873,4.1793]|4.0922|

Old N=16384 reports dependency poll allocation failed, so no speedup ratio.
N=64 and all Scalar ranges are retained in raw CSV. N=256 old metadata peaks
are5432384/5523880 bytes for prefix/integral, compared with1968/11056 Whole;
old payload8208/8464 becomes10008/10264. At N=16384 Whole payload is268056/
270104 bytes and metadata1968/73776. Full collection/output increases sparse
payload cost; exact column state adds width-dependent metadata. These root
counters are not RSS and exclude external input backing.

A12-second128x128 integral capture has11981 execution-chain samples:10966
include execute_scan,6953 ExactAggregate,3454 add_term,3178 round,1130
ResourceBudget::consume and18 collect. Inclusive counts overlap. Largest leaves
are FixedInteger<68>::add3372, exact-ratio top1762, round1302 and memmove1267.
Exact carry/rounding dominates this observed workload; no guessed I/O bottleneck
or floating short-reduction shortcut is asserted.

Raw local files: build/num-whole-remaining/scans-timings.csv,
scans-perf.cpp/build-scans-perf.py, scans-whole.trace/export/sample JSON/profile
summary, public/oracle logs and scans-focused.log.
