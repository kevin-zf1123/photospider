# NUM-06 Result Whole execution

The six formal `numeric.clamp_*` and `numeric.remap_range_*` keys consume tensor members from `Result` inputs and publish one `Result` output named `values`. They use the Whole tensor continuation; a nonempty request reads complete inputs and computes the complete output through its Need and publication stages.

## Tensor contract

Each input port is a `Result` with exactly one tensor member at tensor index zero. Its `ResultTensorSpec::key` may be any key. The operation compares the members' `sample_shape()` values, which include every declared batch axis, and requires identical element types. It does not broadcast or cast inputs.

Clamp accepts UInt8, Int64, Float32 and Float64 with three inputs in order: `input`, `lower`, `upper`. Remap accepts Float32 or Float64 with five inputs in order: `input`, `source_lower`, `source_upper`, `target_lower`, `target_upper`.

Input sample rank is 1 through 8 and the complete logical element count is at most 2^40. Output `values` is a `Result` using schema `photospider.tensor` and tensor key `samples`. Its cell shape is the complete input `sample_shape()`; input batch prefixes become ordinary output axes. Output facets and batch-axis topology are empty. The output retains the input element type.

For every nonempty request, the continuation issues one Need for the complete tensor member on every input with roles 13 (Data, Validation and Descriptor), then publishes the complete output in its next stage. It reads through authorized tensor windows and writes through one direct tensor writer. Whole execution means sparse requested output regions do not reduce the input validation or arithmetic domain. The published Result retains the full output and its global coordinates; query coverage `Q` identifies the observed dependency footprint rather than a packed ROI publication. A bound error outside the requested projection still fails the invocation.

An Empty request creates the correctly typed and shaped Result with empty sample coverage. It issues no input payload Need and performs no sample arithmetic. The callback does not run.

The runtime validates tensor metadata before starting the continuation. Shape or dtype disagreement is a schema error. Typed input semantic validation preserves its original `Status`; it is not converted into a numeric bounds error. Numeric bound failures use `InvalidArgument`, `FailureReason::InvalidDomain`, Domain origin and Run scope, with no Atom key. The diagnostic identifies the bound port and raw bits and appends the full logical coordinate, including batch-prefix axes.

`WholeTensorProgram` reserves bounded host scratch for protocol state and obtains the `RangeMath` workspace through the phase allocator. The callback charges work to the execution root for input collection, each sample, comparison or rational evaluation, and completion. It publishes no partial output on failure. Capacity, work-limit, cancellation and numeric errors release unpublished scratch and payload.

## Clamp behavior

For each logical coordinate, require `lower <= upper`. A NaN lower or upper bound, or a reversed interval, fails the complete invocation. Infinite bounds are valid. Input NaNs do not hide invalid bounds; after bound validation, an input NaN is quieted while retaining its sign and payload bits.

For valid bounds, choose `lower` when `input < lower`, choose `upper` when `input > upper`, and otherwise copy the input bits. Comparisons treat signed zeros as equal, so an in-range `-0` remains `-0`; `clamp(-0,+0,+0)` returns `-0`. This is a direct comparison and bit-selection operation, not a composition of minimum and maximum. Int64 order is compared without converting to floating point, and selected integer or finite floating values are not rounded.

## Remap behavior

For finite ordinary inputs, remap uses

$$
y = t_0 + (x - s_0)\,\frac{t_1-t_0}{s_1-s_0}.
$$

All four bounds must be finite and `source_lower < source_upper`. Invalid bounds fail even when `input` is NaN. Target bounds may increase, decrease or be equal. A constant target interval returns the `target_lower` bits for every non-NaN input.

After validating bounds, quiet an input NaN while retaining its sign and payload bits. Then apply these rules in order: equal target bounds return `target_lower`; an exact `source_lower` endpoint returns `target_lower`; an exact `source_upper` endpoint returns `target_upper`; an infinite input maps to the correctly signed infinity according to the slope; other finite inputs evaluate the full formula as an exact dyadic rational and round once to the output type. A finite mathematical result that overflows produces the correctly signed infinity. Gradual underflow retains the result sign, and an exact non-special zero is positive zero. Endpoint selection preserves the supplied target endpoint bits, including signed zero.

The strict profile rounds the exact formula directly. Accelerated profiles retain the shared [final FP32 four-ULP contract](op_specs/NUM_accelerated_contract.md), including its verified quotient enclosure and strict fallback. They do not introduce NUM-14 certificates, a narrowed Float64 approximation or weaker acceptance bounds.

## Current behavior checks

The maintained Result workflow is [`ranges.cpp`](../../../examples/numeric_workflow/ranges.cpp), built as `photospider_numeric_ranges` and registered as `test_numeric_ranges_result`. It checks a Float64 broadcast-to-remap-to-clamp composition, invalid bounds outside sparse demand, all-port input support including endpoint queries, typed RGB Straight-alpha validation on an unselected channel, Empty, negative/zero/unaligned Int64 layouts, signed and shifted Float32 layouts, and cache identity/invalidation. For sparse `Q`, dirty queries on each input map to the observed output footprint, while the published Result remains complete. The fixture also checks caller and worker floating-environment restoration, resource failures and cancellation cleanup.

The example wraps its original typed backing in source Results charged to the same execution Root; it does not pack a second copy of the inputs. Cache checks distinguish same-Frozen identity reuse from completed-result reuse with fresh source Results, verify that replay records the current source ObjectIds, and confirm that replacing a bound invalidates the cached result. A Result retained after context retirement remains readable, and releasing its final owner returns all Root resource dimensions to zero.

The separate [`test_numeric_result_math.cpp`](../../../tests/integration/test_numeric_result_math.cpp) integration fixture retains additional checks for all four clamp dtypes, integer extrema, batch-axis flattening, negative zero, remap intermediate overflow, pre-cancellation and x86 profile rejection. That broader fixture was not rerun for this Result workflow update. The independent exact Fraction oracle covers UInt8 and Int64 boundaries in addition to the floating cases.

Run the focused public Result workflow test with:

```sh
ctest --test-dir build/kernel-dev -R '^test_numeric_ranges_result$' --output-on-failure
```

The root Result test passed 1/1, and the Apple Silicon default workflow exited successfully. The independent exact Fraction oracle passed 2,826 cases for each of the strict and Apple Silicon profiles. The installed SDK consumer passed 1/1. These checks do not establish x86 execution, native GPU support or a performance result. The separate integration fixture's x86 availability rejection does not validate x86 arithmetic.
