# NUM-10 Whole execution

All eighteen formal concatenate/gather/scatter profile keys execute Whole.
Nonempty demand reads and validates complete active inputs, computes complete
output and then projects to consumers. All input edits invalidate every recorded
output observation. Empty reads nothing. Unselected concatenate ports and
replaced/unused scatter values can now fail input preparation; numerical
selection still copies only the specified values and aggregates only contributors.
Invalid indices and final integer overflow are Domain/Run failures. Public
output keys, shape/dtype, raw selection and stable base-then-increasing-update
NaN priority are preserved. Unsuffixed legacy keys are separate.

Concatenate View requires one affine owner across the complete inputs/output.
Compatible same-owner fragments may join. Independent owners return
ViewUnavailable; select Dense explicitly for them (the helper still defaults
to View). Concatenate remains uncached because content cannot prove physical
viewability. Dense output owns N*dtype_size bytes and Whole collection may own
complete inputs. Gather retains16*M index-plan metadata bytes; scatter stable
radix grouping peaks at32*M, then releases its extra sort vector. Fixed state
is5168 bytes on this build. Coordinate vectors are bounded by rank8 and reused.

## Public workflow and correctness

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric --target photospider_numeric_indexing -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_indexing strict
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_indexing apple
python3 examples/numeric_workflow/index_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_indexing strict
python3 examples/numeric_workflow/index_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_indexing apple
```

Both public suites and3858 independent coordinate/contributor/Fraction oracle
cases per profile pass. Oracle fixtures explicitly reject multi-owner View;
separate shared-owner affine View succeeds. Copy paths preserve raw sNaN bits;
aggregate paths quiet the first contributing NaN and retain zero/Inf rules.
Finite sums use exact accumulation with one final rounding/range check; accelerated
acceptance follows the operator contract, not NUM-14's8uA certificate. Existing
Scalar/NEON/AVX2 paths remain; NUM-14 Scalar/Accelerate/SME are unchanged.

All six families pass work/full-output/scratch budgets (where scratch applies)
and cancellation after admitted work. Index-plan metadata-capacity rejection
releases metadata and payload. Tests include nonleading axes, repeated indices,
unaligned/negative strides, four caller rounding modes, changed-index cache and
Whole support, full typed rejection, Empty/pre-cancel, unselected source failure
and late aggregate overflow cleanup. Five focused numeric/dependency/demand/
resources/compiler CTests, ClangFormat21/cpplint and scoped read-only review pass.
No x86 runtime test was run for this migration; earlier regional WSL validation
is historical.

## Timing and profiler evidence

Apple M5/macOS27.0 (26A5425a), Clang21.1.3 O2/RelWithDebInfo, package0.18/traits16,
-fno-fast-math -ffp-contract=off. Before adapter from451e2b12 is linked to the
same current kernel; this is not a complete historical build comparison.
One worker, result/dependency caches off,1 GiB payload,2^40 dependency work,
512 MiB dependency-state limit. Compile/freeze and numerical checks are outside
timing. One warmup and seven measured calls; every output is checked analytically.

Input/output Int64[N], axis0. Concatenate joins N/2 tens and N/2 ones; gather
reverses0..N-1; scatter starts at10, has N updates of1 with indices[j]=floor(j/2).
Expected first-half sum12, replace/min1, max10, remaining positions10. Core invokes
the actual callback with prepared metadata/prebuilt inputs and includes index-plan
allocation/grouping, output allocation and arithmetic/copy. It excludes public
scheduling/collection and managed work-ledger overhead.

Milliseconds median [min,max], N=16384:

| Operation | Before public | Whole public | Apple callback core | Scalar core median |
| --- | --- | --- | --- | --- |
| concatenate Dense |3.9196 [3.8368,4.0441]|.4397 [.3763,.4757]|.2440 [.2425,.2530]|.2507|
| gather |failed|.8135 [.7543,1.0773]|.6638 [.6400,.6959]|.6326|
| scatter_replace |failed|1.8809 [1.8198,1.9005]|1.5962 [1.3393,1.6290]|1.4952|
| scatter_sum |failed|3.7278 [3.4905,3.9025]|3.3145 [3.2196,3.4460]|3.2945|
| scatter_minimum |failed|2.4943 [2.4325,2.5436]|2.1323 [2.0212,2.1500]|2.1568|
| scatter_maximum |failed|2.5653 [2.5214,2.6612]|2.0850 [1.9363,2.1815]|2.1314|

Old gather/scatter report dependency poll allocation failed under these settings,
so no speedup ratio is claimed. At N=256 all old paths succeed: public medians
concatenate .1598->.0452, gather2.2106->.0509, replace12.4255->.0826,
sum2.8537->.0947, minimum2.9293->.0885, maximum2.8338->.0957 ms.
Full sample ranges for both sizes and Scalar cores are retained in the CSV.
N=16384 controlled payload peaks: concat262144, gather398384, scatter529456
bytes; index-plan metadata is additional. Old concat peak133176. Sparse consumers
pay complete computation/output and may regress. These counters are not RSS.

An initial12-second scatter_sum capture had11977 execution samples,4861 including
resolve_indices and2664 including ResourceBudget::consume. Source inspection
confirmed repeated ledger locking per index/radix item. Index scan and radix
passes now pre-admit at most256 items per block, preserving bounded cancellation
and exact stable order. After correctness passed, seven-call public medians fell
from5.244 to3.728 ms (sum) and3.978 to1.881 ms (replace). Separate captures are
sampling evidence, not per-call normalized CPU counts.

The second12-second capture has11973 execution samples:2338 include
resolve_indices,1220 ResourceBudget::consume,8469 evaluate,5533 ExactAggregate,
and10 collect. Inclusive counts overlap. FixedInteger<68>::add is the largest
leaf with3183 samples, followed by resolve_indices1062. Exact limb arithmetic
remains the numerical cost; no finite-Float32 short-reduction certificate was
extended to this general aggregate.

Raw local evidence: build/num-whole-remaining/indexing-batched-timings.csv,
indexing-timings.csv (pre-batch), indexing-perf.cpp/build-indexing-perf.py,
indexing-whole.trace and indexing-batched.trace, exports/sample JSON/summaries,
strict/Apple oracle/manual logs and indexing-focused-final.log.
