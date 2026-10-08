# NUM-09 Whole execution

The nine formal reshape, transpose and slice profile keys execute CPU Whole
Result programs. Each active input is a single-tensor Result, and its complete
sample_shape() includes any batch axes. The output is a
`photospider.tensor`/`samples` Result with the complete transformed shape as
ordinary axes; output facets and batch-axis metadata are empty. The program
publishes the complete output in global coordinates. A requested query `Q`
limits dependency observation and downstream reads, not the shape or coverage
of the published Result.

The planner validates static schemas and parameters before callbacks. For
nonempty work, the program requests each active tensor with Data, Validation and
Descriptor roles (role 13), which supplies its typed-payload validation
obligation. The callback reads through those authorized windows and computes the
whole output. An edit to observed input support invalidates the recorded output
dependency. Empty output has empty coverage and support. Slice ignores a
singleton axis's step value. When every count is one, the step port is excluded
from runtime Need and association; its static schema is still checked.
Otherwise all step entries are requested and validated, even where a particular
entry is numerically unused. Upstream, typed, domain, resource and cancellation
failures remain visible to the Run. The transform preserves selected raw
element bits, including signaling-NaN payloads.

`View` requires one affine source/output owner proven for the complete output.
Compatible fragments can join only when they share an owner and the address
mapping remains valid. Multiple owners make explicit `View` unavailable. `Auto`
uses a view when possible and otherwise materializes a complete packed output;
`Dense` always materializes that complete output. Only `ViewUnavailable`
permits `Auto` to fall back. The operation reads from authorized source windows
and does not need to pack the complete input first.
Resource, validation, upstream and cancellation errors remain errors. A sparse
query cannot make a globally non-affine view valid. Reshape proves contiguous
logical chunks, transpose permutes strides, and slice uses checked widened
stride arithmetic. Views retain source storage and resources after context
retirement. All three operations disable cross-run content caching because a
content key does not prove owner or stride identity; same-run sharing remains
available.

## Public workflow and validation

```sh
cmake --build build/kernel-dev --target photospider_numeric_layouts -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_layouts_result$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_layouts strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_layouts apple
python3 oracle/ops/numeric/layout_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_layouts strict
python3 oracle/ops/numeric/layout_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_layouts apple
```

The focused `test_numeric_layouts_result` test and the full default workflow pass
on the local Apple host. The independent seven-dtype oracle passes 1,113 cases
for each of the strict and Apple profiles. The separate finite-address oracle
checks 6,374 successful View/Auto/Dense mappings per profile plus expected View
rejections. It independently covers negative, zero and singleton strides,
unaligned addresses and non-contiguous chunks. No current x86 execution, native
GPU execution or performance result is claimed. The installed consumer test
`installed_numeric_layouts_result` also passes 1/1 against the installed
`Photospider::kernel` package.

Public graph fixtures test compatible same-owner fragments, multi-owner
View failure and Auto/Dense output materialization, full source support/dirty, invalid full
slice controls, excluded all-singleton step producers, complete typed rejection,
Empty, work/output/scratch limits and cancellation after admitted copying.
Raw specials in all four fenv modes and escaped storage/resource lifetime pass.
Prepared Whole view fixtures include giant zero-stride producer output,
structured consumption, original external dense owner retention, multi-owner
rejection, same-owner singleton joining and sticky allocator failures.
The separate [`prepared.cpp::whole_views` Result fixture](../../../examples/numeric_workflow/README.md#whole-result-view-preparation)
uses a compiled CPU Whole Tensor Need: it checks affine view proof, strict
multi-owner rejection, Auto collection into Root-owned private backing, an
8-byte source under an 8-byte output cap, and owner lifetime after context
retirement. It documents a Result input path, not the legacy NUM-09 oracle or
timing evidence below.
Earlier measurements of a 288-byte fixed workspace belong to the Value Whole
implementation. The current Result operation declares zero fixed workspace
bytes and reserves up to 4096 bytes of Host scratch for rank<=8 coordinate
vectors. Earlier regional checks do not establish Whole behavior. The installed
consumer is registered as `installed_numeric_layouts_result`; run it from the
configured consumer build after installing the SDK:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
cmake -S tests/consumer -B build/kernel-dev/consumer-build -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/kernel-dev/consumer-build --target photospider_numeric_layouts_consumer -j8
ctest --test-dir build/kernel-dev/consumer-build -R '^installed_numeric_layouts_result$' --output-on-failure
```

## Historical Value Whole timing and bottleneck evidence

The following measurements describe the earlier Value Whole implementation,
not the current Result workflow. They were recorded on Apple M5, macOS27.0
(26A5425a), Clang21.1.3, O2/RelWithDebInfo, with `-fno-fast-math` and
`-ffp-contract=off`, package0.18/traits16. The earlier adapter was linked to a
later kernel; this isolates adapter behavior, not an entire historical build.
It used one worker, result/dependency caches disabled,
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

Raw local files from that historical run are not part of the current Result
acceptance. The timing table remains only as historical Value Whole evidence.
