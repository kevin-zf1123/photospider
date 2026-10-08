# NUM-11 Whole execution

The 21 formal `numeric.reduce_*` keys accept one Result per input port, with exactly one tensor member under any member key. The complete `sample_shape()` includes batch axes; rank is 1..8, extents are positive, and the logical element count is at most 2^40. Each `values` output is a `photospider.tensor` / `samples` Result. It preserves rank, sets reduced extents to one, uses ordinary axes, and drops input facets and batch topology.

The six numerical reducers validate static schema and parameters during specialization. For nonempty demand, Whole requests the input with Data, Validation and Descriptor roles (role 13), validates its typed payload, and reads authorized Result windows. It computes every keepdims group and publishes a complete dense output before consumer projection. The implementation does not first collect or pack the full input. A typed-validation failure or arithmetic overflow in an unrequested group still fails the selected output. Empty demand reads no payload and performs no group arithmetic. Any active source edit dirties the complete output.

`reduce_count` specializes with an empty runtime input projection. It still validates the complete static schema and axes, but creates no runtime source observation or association and does not schedule an upstream sample producer. The producer binds a known-empty support witness, multiplies the reduced extents in O(rank), and publishes the complete keepdims shape through zero strides over one 8-byte Int64 owner. Source-byte edits do not invalidate count; schema shape/type and axes determine it.

## Memory and errors

The six numerical reducers hold authorized input windows while owning a complete dense output and one fixed exact aggregate or moments workspace. The workspace is admitted through the phase allocator. The Root ledger accounts for read windows, output, exact state, work and cancellation. Capacity, work, upstream, typed-validation and cancellation failures preserve their host status and release unpublished output. Integer final overflow is a Domain/Run `ArithmeticOverflow` failure at its output coordinate; floating infinity and NaN are numerical results.

Count's output size is independent of its logical shape. Its 8-byte zero-stride backing remains readable after context retirement and is released with its last owner. Managed resource counts do not bound process RSS.

## Current behavior checks

Build and run the public Result workflow and the independent Fraction/midpoint-square oracle with:

```sh
cmake --build build/kernel-dev --target photospider_numeric_reductions -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_reductions strict
python3 oracle/ops/numeric/reduction_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_reductions strict
python3 oracle/ops/numeric/reduction_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_reductions apple
ctest --test-dir build/kernel-dev -R '^test_numeric_reductions_result$' --output-on-failure
```

The strict and available Apple Silicon workflow runs exit successfully. The oracle covers 4,740 independent cases per profile. The workflow exercises all seven reducers under four rounding modes on the actual worker and caller, typed validation, strided inputs, unrequested-group overflow, Empty demand, work and cancellation failures, and invalid `ddof`.

A generated Result producer publishes 4,096 values cycling through 0..3. Sum, mean, variance and standard deviation return 6,144, 1.5, 1.25 and the correctly rounded square root of 1.25. The test reads the Root Payload peak directly and requires it to include at least the 32 KiB source while staying within the configured 128 KiB cap. After context retirement, the source has retired and the retained dense reduction Result accounts for 8 bytes; releasing it returns Root resources to zero.

A two-stage tail-failure case publishes and certifies the first 128 samples of a 129-sample source, including an sNaN, then returns the original `required tail after NaN` `OperationFailed` on poll two. All six numerical reductions fail with that upstream error before any numerical computation poll. The failed source prefix remains readable after context retirement, while coordinate 128 has no coverage; releasing the prefix returns Root resources to zero.

The count case derives a `[2^20,1]` output from a `2^40`-element schema while leaving its failing sample producer factory uncalled. It records no source observations or association. The first and last logical elements share a pointer in the zero-stride 8-byte backing; an escaped read window remains valid after context retirement, and the final release returns Root resources to zero.

The cache case distinguishes identity sharing on one Frozen workflow from completed-result reuse with a fresh equal-content source and Frozen workflow. The cache replay carries the current source ObjectId. Replacing an unrequested input group invalidates and recomputes the complete Whole result; the requested first group remains 3 while the second becomes 11. The original dense output stays readable after context retirement at 16 payload bytes and releases all Root resources with its last owner.

Each Whole write keeps one `NumericDiagnostics` record local to the callback. `evaluated_values` increments after an authorized source word is read and immediately before it is supplied to the exact accumulator; on success it counts accumulator input attempts, not output groups. `reduce_count` uses a metadata-only path and reports zero evaluated values. The selected profile and exact accumulator implementation identity are reported once at completion. The identity's ISA suffix describes the four-word output-selection helper, not SIMD execution of the reduction algorithm. Strict and Apple workflow checks report zero strict-math calls, strict fallbacks and fallback reasons.

The callback reports diagnostics after normal completion or a status it handles locally, preserving the first operation failure if reporting is rejected. WorkLimit or cancellation can prevent the report from merging. A `Status` thrown during tensor-window acquisition/read or `std::bad_alloc` caught by the outer math callback can bypass this local report. Cache hits add no new accumulator attempts; Empty demand does no sample arithmetic.

The final focused root selection completed 40 tests in 19.79 seconds: 39 passed and one skipped, with zero failures. It included this dedicated test and the shared `test_numeric_result_math` fixture. Root resource tests passed 3/3, and the installed numeric selection passed 10/10. The independent reduction oracle passes 4,740 cases per Strict and Apple profile. Sibling-source 4 MiB and 5 MiB Root Payload peak assertions passed in the complete `test_dependency_program` run. The skipped test was `test_vulkan_gpu`, with return code 77. x86 execution, Vulkan execution and new performance measurements were not run.

Typed-boundary checks use a straight-alpha RGB Result. A green-only request still validates the complete typed input, so an invalid alpha outside that selected channel fails each of the six numerical reducers with `InvalidArgument` / `InvalidDomain`; metadata-only count succeeds without reading it. Empty demand has empty coverage, support and computed-element count. Pre-cancellation, same-schema valid typed input, logical-order NaN priority across strided windows, unaligned/negative/zero strides, final overflow, and invalid `ddof` are also checked.

The installed consumer uses the same workflow source and links `Photospider::kernel`. To check it against a local install, run:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-repeat-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/result-repeat-install" \
  -DPhotospider_DIR="$PWD/build/kernel-dev/result-repeat-install/lib/cmake/Photospider" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_reductions_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_reductions_result$' --output-on-failure
```

The installed selection above includes `installed_numeric_reductions_result`.
Its compile uses only the installed prefix's `include` directory plus
`-fno-fast-math -frounding-math -ffp-contract=off`, and links the prefix's
`lib/libphotospider.a`.

These checks do not establish x86 execution, GPU support, maximum-shape throughput or an RSS bound. Historical Value-path measurements are not evidence for the current Result implementation. The seven specifications remain Proposed; their formulas and acceptance rules are linked from [the shared contract](op_specs/NUM-11_reduction_contract.md).
