# NUM-09 Whole execution

The nine formal reshape/transpose/slice profile keys execute Whole callbacks.
All active inputs are collected/validated for nonempty demand, and the complete
output is published before projection. All active input edits invalidate the
complete output; upstream/typed/domain failures affect the Run. Empty reads
nothing. Slice ignores singleton-axis steps numerically; the complete step port
is excluded only when all counts are one. Otherwise even unused step entries
participate in Whole input preparation. Output names, shapes, dtypes and exact
raw bits are unchanged, including sNaN payloads. Legacy unsuffixed keys are
separate. No NUM-14 numeric certificate is reused.

View requires one complete affine input/output owner. Compatible same-owner
fragments may join after an address-map proof in both execution bridges.
Multiple owners fail View with ViewUnavailable; Auto/Dense may collect. Reshape
proves maximal contiguous chunks symbolically; transpose permutes strides; slice
uses checked widened stride multiplication. A small projection cannot make a
globally non-affine View succeed. Auto falls back only for unavailable views.
Dense allocates N*dtype_size output bytes plus fixed state, and may own full
collected inputs; View retains source storage/resources, possibly oversized.
All three retain cacheable=false because content caches do not witness layout.

## Public workflow and validation

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric --target photospider_numeric_layouts photospider_numeric_prepared -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_layouts strict
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_layouts apple
python3 examples/numeric_workflow/layout_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_layouts strict
python3 examples/numeric_workflow/layout_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_layouts apple
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_prepared
```

Both profiles passed public examples (`[2,3]->[3,2]` reshape, transpose
`values[k,i,j]=100*i+10*j+k`, reverse slice `[4,2,0]`) and 636 independent
integer-coordinate/raw-bit oracle cases each. A separate exhaustive finite
address oracle, independent of production chunk rules, passed 6374 successful
View/Auto/Dense cases per profile plus expected View rejections. It covers
negative/zero/singleton strides, unaligned origins and non-contiguous chunks.

Public graph fixtures test compatible same-owner fragments, multi-owner
View failure/Auto/Dense collection, full source support/dirty, invalid full
slice controls, excluded all-singleton step producers, complete typed rejection,
Empty, work/output/scratch limits and cancellation after admitted copying.
Raw specials in all four fenv modes and escaped storage/resource lifetime pass.
Prepared Whole view fixtures include giant zero-stride producer output,
structured consumption, original external dense owner retention, multi-owner
rejection, same-owner singleton joining and sticky allocator failures.
The fixed workspace is 288 bytes on this arm64 build. Rank<=8 coordinate
vectors use bounded host-container allocation. No x86 runtime test was performed;
older 2026-09-14 regional WSL checks are not Whole acceptance.

## Timing and bottleneck evidence

Apple M5, macOS27.0 (26A5425a), Clang21.1.3, O2/RelWithDebInfo,
-fno-fast-math -ffp-contract=off, package0.18/traits16. Old adapter is from
63f9d794, linked to the same current kernel; this isolates adapter behavior,
not an entire historical build. One worker, result/dependency caches disabled,
1 GiB payload limit, 2^40 dependency work and 512 MiB dependency-state limit.
Compile/freeze happen before timing. Each row has one warmup and seven measured
calls; every output is checked outside timing. Input Int64[N/2,2]=0..N-1;
reshape outputs [N], transpose [2,N/2], slice reverses both axes with full counts.

Milliseconds, median [min,max], N=16384:

| Operation/layout | Before public | Whole public | Apple callback core | Scalar core median |
| --- | --- | --- | --- | --- |
| reshape/View | failed | .0346 [.0333,.0541] | .000542 [.000500,.000917] | .000666 |
| reshape/Dense | failed | .4072 [.3983,.4246] | .2798 [.2673,.3036] | .2563 |
| transpose/View | .1068 [.0929,.1194] | .0403 [.0354,.0608] | .000625 [.000583,.002375] | .000708 |
| transpose/Dense | 1.4908 [1.4358,1.6296] | .3902 [.3838,.4136] | .2059 [.1965,.2190] | .2360 |
| slice/View | failed | .0495 [.0409,.0656] | .001250 [.000583,.002000] | .001250 |
| slice/Dense | failed | .3740 [.3430,.4275] | .2338 [.2303,.2619] | .2395 |

The old reshape/slice adapter reports dependency poll allocation failed at this
size under the stated budget; no speedup ratio is claimed for those rows.
At N=256 it succeeds: reshape Dense public1.389->.0452 ms; slice Dense
3.638->.0474 ms. Full min/max ranges for both sizes/profiles are in the CSV.
Core directly executes the actual callback with prepared metadata and prebuilt
inputs, including allocation/mapping/copy and excluding public scheduling,
collection and managed work-ledger overhead. This is a copy core, not a claim
about floating arithmetic throughput. Scalar and platform-specific copy paths
remain; NUM-14 Scalar/Accelerate/SME implementations are unchanged.

At N=16384, Whole Dense controlled peaks are 262432 bytes (reshape/transpose)
and 262464 (slice), including full collected inputs and output plus workspace.
Old transpose Dense peaks at131464 bytes. Whole View peak is288 bytes of new
workspace, excluding retained external source131072 bytes and controls. These
are controlled allocations, not total RSS. Sparse Dense consumers still pay
full output computation/storage; small sparse requests may regress.

A 12-second Time Profiler capture of Whole Dense transpose includes 11840
execution-chain samples:10744 include execute_layout,9392 LayoutState,
2191 ResourceBudget::consume,670 byte_address and92 collect. Inclusive counts
overlap. Top leaves include address calculation1731, memmove1553, mutex
lock757/unlock592, LayoutState::execute625 and map560. Mapping/copy and managed
work metering dominate this measured case; full input collection is a small
sample fraction. No unmeasured claim is made about other shapes.

Raw local files: build/num-whole-remaining/layouts-timings.csv, layouts-perf.cpp,
build-layouts-perf.py, layouts-whole.trace/XML, layouts-samples.json and profile
summary, manual/oracle logs and layouts-focused.log. Focused verification and
ClangFormat21/cpplint results accompany the local commit.
