# NUM-12 Whole execution

The six formal sort/quantile keys use Whole. Every nonempty selected output reads
and validates all active inputs, computes its complete output and then projects.
Any active input edit invalidates all recorded observations; source/typed/domain
failures affect the Run. Each sort output has an independent callback and only
allocates that complete values or indices array. Quantile excludes q by static
projection when axis length is1, while still checking its schema. Otherwise
source failure can precede invalid-q callback validation. Empty reads nothing.
Public output identities, stable ties, raw sort values, NaN priority and exact
quantile position/final rounding are preserved. No NUM-14 certificate is used.

Per line, stable heapsort orders(numerical key, original index). Numerical keys
are classified once and stored separately, avoiding repeated strided reads in
comparisons. Key/permutation element storage is16*L bytes plus two allocator
headers/alignment and Entries reservations, charged as metadata. Keys are released
after sorting, permutation after copying that line. Fixed state is4008 payload
bytes on this build; complete input/output payloads are additional. The former
16-times-input workspace admission and staged block/permutation sharing are
removed. Scalar/NEON/AVX2 comparisons remain; NUM-14 Accelerate/SME are unchanged.

## Workflow and validation

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric --target photospider_numeric_ordering -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_ordering strict
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_ordering apple
python3 examples/numeric_workflow/ordering_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_ordering strict
python3 examples/numeric_workflow/ordering_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_ordering apple
```

Both public suites and2072 independent stable-order/Fraction quantile cases per
profile pass, including after key caching. Sort[3,1,1,2] gives values[1,1,2,3],
indices[1,2,3,0]; quantile([0,10,20,30],.25)=7.5. Cases cover dtype extremes,
NaN payloads, signed-zero stable ties, exact endpoints/interpolation, nonlast
axis lines, sparse/joint/separate outputs, cache replacement, q exclusion and
failure, full typed validation, negative strides, four fenv modes/flags and Empty.
Both sort outputs and quantile pass work/output/scratch and active cancellation
checks. Permutation metadata exhaustion releases state/output/metadata. Generic
public block-contract probes remain separately tested, without claiming that
these operators use blocks. Five focused CTests and format/lint pass; scoped
independent review found no required issue. No x86 runtime validation here.
Older regional WSL/installed records are historical.

## Public and core timing

Apple M5/macOS27.0 (26A5425a), Clang21.1.3 O2/RelWithDebInfo,
-fno-fast-math -ffp-contract=off, package0.18/traits16. Before adapter and its
stable_order header fromacd8a31a link to the same current kernel. One worker,
result/dependency caches off,1 GiB payload,2^40 dependency work,512 MiB dependency
state. One warmup and seven measured calls; compile/freeze and complete analytic
output checks are outside timing. Int64 input[4,N/4], each row is descending
N/4-1..0, axis1. Values must ascend, indices must descend; quantile q=.25 gives
Float64(N/4-1)/4 per row.

Core invokes the actual callback with prepared metadata/prebuilt input, including
key/permutation/output allocation and arithmetic/copy, excluding public scheduling,
collection and managed work-ledger overhead. It is not an allocation-free sort
microkernel. N=16384, milliseconds median[min,max]:

| Output | Before public | Whole public | Apple callback core | Scalar core median |
| --- | --- | --- | --- | --- |
| sort values |50.9162[50.7298,51.1899]|6.4472[6.3567,6.8370]|4.3784[4.3403,4.7985]|4.4922|
| sort indices |50.7482[50.2351,51.7351]|6.1929[6.1198,6.2225]|4.0680[3.9860,4.1075]|4.1720|
| quantile |45.9069[45.4385,52.1802]|5.6186[5.6045,5.6368]|3.9130[3.7727,3.9485]|4.0552|

At N=256, public medians are values .4805->.0979, indices .4661->.0842 and
quantile .7215->.1063 ms. Full Scalar/small-size ranges remain in raw CSV.
N=16384 controlled payload peaks are266152 bytes for either sort output and
135120 for quantile, versus168112/37080 before. Actual root metadata peaks are
67760/68248 bytes versus19720/53208 before. These include allocator/host metadata,
not just element arrays; externally supplied backing and RSS are separate.
Sparse demands still pay complete selected output and all-line sorting.

An initial12-second Whole sort-values capture has11969 execution samples,
including11624 StableOrderWorkspace and2382 ResourceBudget::consume. Leaf samples
include address calculation1367 and memmove1439; code inspection showed repeated
strided source reads inside every comparison. After caching each numerical key
once, all correctness tests passed and public median decreased from20.893 to
6.447 ms (same N/settings), at the cost of one additional8*L key array.

The second12-second capture has11990 execution samples:10744 include ordering,
3268 ResourceBudget::consume,51 byte_address and8 collect. Largest named leaves
include mutex lock1233/unlock1094 and the sorting lambda1013. Inclusive counts
overlap and captures cover different numbers of executions; fractions are not
per-call CPU comparisons. Remaining comparison/work-metering costs are measured,
and no I/O bottleneck is inferred from them.

Raw files: build/num-whole-remaining/ordering-key-timings.csv,
ordering-timings.csv(before key caching), ordering-perf.cpp/build-ordering-perf.py,
ordering-whole.trace/ordering-key.trace and exports/sample JSON/summaries,
manual/oracle logs and ordering-focused.log.
