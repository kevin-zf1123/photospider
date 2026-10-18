# NUM-11 Run-shared complete-input computation

The 21 formal `numeric.reduce_*` keys accept one Result per input port, with exactly one tensor member under any member key. The complete `sample_shape()` includes batch axes; rank is 1..8, extents are positive, and the logical element count is at most 2^40. Each `values` output is a `photospider.tensor` / `samples` Result. It preserves rank, sets reduced extents to one, uses ordinary axes, and drops input batch topology.

The six numerical reducers validate static schema and parameters during specialization. Each key is a `sdk::with_whole_stage` operation: the planner grants the stage every tile of the projected input, the explicitly bound validators run on that grant before the first poll, and every requested output tile reads the complete stage Result. The stage validates its typed payload and reads authorized Result windows in one poll. It computes every keepdims group and publishes a complete dense output before consumer projection. The implementation does not first collect or pack the full input. A typed-validation failure or arithmetic overflow in an unrequested group still fails the selected output. Empty demand reads no payload and performs no group arithmetic. Any active source edit dirties the complete output.

`reduce_count` specializes with an empty runtime input projection. It still validates the complete static schema and axes, but its stage mapping grants no input tile, so it creates no source observation or association and the planner emits no source-producer instruction on its behalf. The producer binds a known-empty support witness, multiplies the reduced extents in O(rank), and publishes the complete keepdims shape through zero strides over one 8-byte Int64 owner. Source-byte edits do not invalidate count; schema shape/type and axes determine it.

## Memory and errors

The six numerical reducers hold authorized input windows while owning a complete dense output and one fixed exact aggregate or moments workspace. The workspace is admitted through the phase allocator. The Root ledger accounts for read windows, output, exact state, work and cancellation. Capacity, work, upstream, typed-validation and cancellation failures preserve their host status and release unpublished output. Integer final overflow is a Domain/Run `ArithmeticOverflow` failure at its output coordinate; floating infinity and NaN are numerical results.

Count's output size is independent of its logical shape. Its 8-byte zero-stride backing remains readable after context retirement and is released with its last owner. Managed resource counts do not bound process RSS.

## Current behavior checks

Use independent Fraction and midpoint-square references for each admitted profile. Temporary acceptance should exercise all seven reducers under four rounding modes on the actual worker and caller, typed validation, strided inputs, unrequested-group overflow, Empty demand, work and cancellation failures, and invalid `ddof`.

A generated Result producer publishes 4,096 values cycling through 0..3. Sum, mean, variance and standard deviation return 6,144, 1.5, 1.25 and the correctly rounded square root of 1.25. The test reads the Root Payload peak directly and requires it to include at least the 32 KiB source while staying within the configured 128 KiB cap. After context retirement, the source has retired and the retained dense reduction Result accounts for 8 bytes; releasing it returns Root resources to zero.

A two-stage tail-failure case publishes and certifies the first 128 samples of a 129-sample source, including an sNaN, then returns the original `required tail after NaN` `OperationFailed` on poll two. All six numerical reductions fail with that upstream error before any numerical computation poll. The failed source prefix remains readable after context retirement, while coordinate 128 has no coverage; releasing the prefix returns Root resources to zero.

The count case derives a `[2^20,1]` output from a `2^40`-element schema while leaving its failing sample producer factory uncalled. It records no source observations or association. The first and last logical elements share a pointer in the zero-stride 8-byte backing; an escaped read window remains valid after context retirement, and the final release returns Root resources to zero.

The cache case distinguishes identity sharing on one Frozen workflow from completed-result reuse with a fresh equal-content source and Frozen workflow. A cache hit is keyed by the granted input content and rebinds the cached output with a new ObjectId. Replacing an unrequested input group changes that content and recomputes the complete complete-input result; the requested first group remains 3 while the second becomes 11. The original dense output stays readable after context retirement at 16 payload bytes and releases all Root resources with its last owner.

Each complete-input write keeps one `NumericDiagnostics` record local to the callback. `evaluated_values` increments after an authorized source word is read and immediately before it is supplied to the exact accumulator; on success it counts accumulator input attempts, not output groups. `reduce_count` uses a metadata-only path and reports zero evaluated values. The selected profile and exact accumulator implementation identity are reported once at completion. The identity's ISA suffix describes the four-word output-selection helper, not SIMD execution of the reduction algorithm. Do not infer strict-math or fallback counts from the selected ISA suffix.

The callback reports diagnostics after normal completion or a status it handles locally, preserving the first operation failure if reporting is rejected. WorkLimit or cancellation can prevent the report from merging. A `Status` thrown during tensor-window acquisition/read or `std::bad_alloc` caught by the outer math callback can bypass this local report. Cache hits add no new accumulator attempts; Empty demand does no sample arithmetic.

Generic numeric reducers do not install an RGBA or alpha-association validator from a four-channel shape or image schema. Temporary checks use each reducer's numerical rules and explicitly bound contracts, including nonfinite values, signed/zero strides, overflow, ddof, cancellation and empty payload demand.

Compile temporary numerical checkers with
`-fno-fast-math -frounding-math -ffp-contract=off`.

These checks do not establish x86 execution, GPU support, maximum-shape throughput or an RSS bound. The seven specifications remain Proposed; their formulas and acceptance rules are linked from [the shared contract](op_specs/NUM-11_reduction_contract.md).
