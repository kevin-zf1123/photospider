# NUM-15 Whole execution

All six formal derivative_1d/integrate_1d profiles execute Whole. Nonempty demand
collects/validates all active inputs, computes complete output, then projects.
Source failure may precede callback step validation. Invalid zero/nonfinite step
is Domain/Run; negative finite step remains valid. Any active input edit
invalidates all recorded observations. Empty reads nothing.

Derivative retains adjacent endpoint and two-neighbor interior stencils; a center
NaN still does not enter its own interior result, although Whole reads it for
preparation/other outputs. Integral retains exact unweighted prefix, first/last
samples and source classifications. A separate exact ratio computes the weighted
area plus initial with one final rounding. Output0 preserves initial raw bits,
including sNaN and signed zero. Only N=1 statically excludes samples and step
(projection{2}); all metadata remains checked. N>1 output0-only requests still
require complete inputs and valid step. Public keys, output identity and numerical
rules are unchanged. Legacy keys remain separate.

Fixed ExactCalculus state is5768 payload bytes on this build. N*dtype_size output
and full collected active inputs are additional. N=1 copies initial without
constructing arithmetic state, while registration conservatively retains the
common workspace admission bound. One reusable rank-one coordinate replaces
per-read vector allocation; no point plan, source-set descriptor, continuation
or per-value numeric counter remains. Scalar/platform exact arithmetic and
NUM-14 Scalar/Accelerate/SME comparisons remain; no8uA shortcut is generalized.

## Public workflow and independent validation

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric --target photospider_numeric_calculus -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_calculus strict
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_calculus apple
python3 examples/numeric_workflow/calculus_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_calculus strict
python3 examples/numeric_workflow/calculus_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_calculus apple
```

Both public suites and1810 independent Fraction/raw-bit oracle cases per profile
pass. Derivative([0,1,4],step1)=[1,2,3]; integrate([0,1,2],step1,initial0)=
[0,.5,2]. The oracle retains each profile's actual numerical acceptance contract.
Tests cover N=2/endpoints, exact cancellation and overflow/underflow, tiny/negative
step, raw initial versus quieted positive results, source/initial NaN priority,
center NaN exclusion, singleton failed-producer exclusion, full source/step
failure scope, sparse dirty/cache-off behavior, negative/zero/unaligned layouts,
four fenv modes/flags, incompatible typed metadata, schema/Empty/pre-cancel,
work/full-output/scratch limits and cancellation after admitted arithmetic with
payload cleanup. A4096-input sparse integral projection computes complete output.

Final seven focused numeric/matrix-certificate/dependency/demand/fragments/
resources/compiler CTests pass. NUM-14 strict/Apple public workflows and1598
Apple independent affine cases pass on the resulting kernel. Formatting/lint
and scoped read-only review pass. No x86 runtime validation for this migration;
older regional WSL/installed records are historical.

## Public latency and numerical core

Apple M5/macOS27.0 (26A5425a), Clang21.1.3 O2/RelWithDebInfo,
-fno-fast-math -ffp-contract=off, package0.18/traits16. Before adapter fromc9e6c826
links to the same current kernel. One worker, result/dependency caches off,
1 GiB payload,2^40 dependency work,512 MiB dependency state. One warmup plus
seven measured calls; compile/freeze and complete analytic checks are outside
timing. Float64 samples[N]: derivative uses i*i, step1, expecting endpoints1/
2*N-3 and interiors2*i; integral uses i, step1, initial2, expecting2+i*i/2.

Core invokes the actual callback with prepared metadata/prebuilt input, including
state/output allocation, exact arithmetic and stores, excluding public scheduling,
collection and managed work-ledger overhead. Milliseconds median[min,max]:

| N / operation | Before public | Whole public | Apple callback core | Scalar core median |
| --- | --- | --- | --- | --- |
|256 / derivative|2.6815[2.6277,2.7216]|.1327[.1258,.1538]|.0802[.0794,.0905]|.0907|
|256 / integral|5.7530[5.5561,6.1187]|.2677[.2443,.3078]|.1904[.1865,.1985]|.1946|
|16384 / derivative|failed|7.0967[7.0007,7.1137]|6.5577[6.3362,6.9515]|6.5625|
|16384 / integral|failed|14.6578[14.5527,14.7712]|12.7898[12.6116,12.8841]|12.8743|

Old N=16384 reports dependency poll allocation failed; no speedup ratio is claimed.
N=16 and full Scalar ranges remain in raw CSV. N=256 old metadata peaks5253768/
9819832 bytes become2456/2944 for derivative/integral; payload8040 becomes9872/
9880. N=16384 Whole payload is267920/267928 bytes with metadata2456/2944.
These counters exclude external input backing/RSS. Sparse requests incur complete
computation/output and can regress despite reduced descriptor overhead.

A12-second integral capture contains11976 execution-chain samples:11902 include
execute_calculus,10166 ExactCalculus::integral,4534 multiply_fixed,1024 round,
1052 ResourceBudget::consume and4 collect. Inclusive counts overlap. Largest
leaves include multiply_fixed2258, FixedInteger::set1679, subtract1365 and
add1012. Exact weighted-prefix arithmetic dominates the observed case; no
unmeasured I/O bottleneck or limited-domain numeric shortcut is asserted.

Raw local files: build/num-whole-remaining/calculus-timings.csv,
calculus-perf.cpp/build-calculus-perf.py, calculus-whole.trace/export/sample JSON/
summary, strict/Apple oracle/manual logs, calculus-focused.log and final-matrix logs.
Final registry audit records213 formal numeric/array profile keys and225 output
declarations, all Whole with zero dependency_version/continuation_bytes, in
formal-audit.csv. This excludes unsuffixed legacy operators.
