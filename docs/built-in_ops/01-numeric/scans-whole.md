# NUM-13 Whole execution

The six `numeric.prefix_sum_*` and `numeric.integral_image_*` operations execute
as Whole Result programs. Each operation accepts one tensor member under any
input key. The output port key is `values`; its Result schema is
`photospider.tensor` with tensor member `samples`. A nonempty output demand
requests the complete input support with Data, Validation and Descriptor (role
13), reads through authorized Result windows, computes every output coordinate,
and publishes the complete packed Result in global coordinates. The requested
footprint scopes observed dependency roots. The returned Result retains its
complete published coverage, and consumers can read any covered coordinate; the
request does not turn it into a packed ROI. The output starts from the complete
input `sample_shape()`; each selected scan axis grows by one, and those extents
form ordinary output axes with facets and batch topology dropped. Empty demand
schedules no input payload work and publishes empty tensor coverage.

`numeric_tensor_program.hpp` supplies the Whole protocol and the operation's
Need-authorized tensor read window. The scan kernel receives that read capability
and one packed Result writer. The writer borrows its window for publication; the
Result owns immutable output storage after execution context retirement. The
input reader follows logical coordinates through the authorized row runs, so
negative and noncontiguous source strides remain supported.

`prefix_sum` keeps an exact source aggregate for each line and copies it to a
separate conversion snapshot before converting each boundary. The leading zero
boundary is written as integer zero or floating +0. That padding value does not
participate in later sums. A generated NaN from an earlier prefix is not treated
as a source NaN by later prefixes; source NaN priority follows logical axis order.

`integral_image` treats unselected axes as independent planes. For each plane,
the kernel processes the lower-numbered selected axis as rows and the higher-
numbered axis as columns. At each source cell it adds the current row's exact
prefix to the saved exact rectangle from preceding rows. These disjoint terms
form the rectangle anchored at the zero boundaries. Each column carry stores the
exact magnitude and sign, first source NaN, positive and negative infinity flags,
and all-negative-zero state. It is saved before final conversion can reuse the
aggregate's arithmetic scratch, and all columns reset for each plane.

Both kernels check work and cancellation while reading, updating carries,
converting, and publishing. Integer destination overflow at any complete output
coordinate fails the Whole Run with that coordinate, even when it lies outside
the requested projection. Floating overflow is an output infinity and does not
alter the exact carry used for later outputs. Failures discard unpublished
output and kernel state. A WorkLimit failure releases output payload during the
run; resource metadata and allocator entries return to the source Result
baseline when the execution context is destroyed.

## Public use and focused validation

The public C++ helpers `prefix_sum_node` and `integral_image_node` are declared
in `include/photospider/numeric/scans.hpp`. This snippet assumes the caller has
already created a `WorkflowInput` and a workflow builder:

```cpp
auto prefix = ps::numeric::prefix_sum_node(
    1, input, 0, ps::ElementType::Int64);
auto integral = ps::numeric::integral_image_node(
    2, input, {0, 1}, ps::ElementType::Int64);
```

The current public workflow entry is `examples/numeric_workflow/scans.cpp`,
registered as `test_numeric_scans_result`; the same source is built as the
installed consumer `installed_numeric_scans_result`. It exercises independent
small-integer prefix and rectangle enumeration, nonadjacent selected axes,
batch axes, negative and unaligned strides, NaN and infinity ordering, exact
cancellation, negative zero, typed validation at zero boundaries, Empty demand,
unrequested integer overflow, resource limits, cancellation and Result lifetime.
An integral-image request for the representable value at `[2,2]` still fails
`Domain/Run` because the complete output overflows at unrequested `[1,2]`; the
failure has no Atom key, while its diagnostic identifies global output
coordinate `[1,2]`; Root resources return to zero.
The current tests also cover a generated 4,096-sample Result source, a
stable-prefix source that fails after publishing its first 128 samples, source
replacement and completed-result cache reuse. They check that edits dirty the
observed output request, including zero boundaries, and that an escaped output
window remains readable after the context and Result handle retire. Input is
read through authorized windows; the fixture does not pack the complete source
into a second payload. The generated-source case verifies one scan over all
4,096 values, complete 4,097-element output coverage and the output's 32,776-byte
payload after context retirement. The stable-prefix case confirms that a
zero-boundary query still waits for the complete source Need and propagates its
tail failure before performing scan arithmetic.
Each Whole write keeps one `NumericDiagnostics` record local to the callback. `evaluated_values` increments after an authorized source word is read and immediately before it is supplied to the exact accumulator; on success it counts accumulator input attempts, not output positions. Prefix zero boundaries, carry snapshots and final output conversions do not increment it. The selected profile and exact scan accumulator identity are reported once at completion. Its ISA suffix describes the four-word output-selection helper, not SIMD execution of the scan algorithm. Strict and Apple workflow checks report zero strict-math calls, strict fallbacks and fallback reasons.

The callback reports diagnostics after normal completion or a status it handles locally, preserving the first operation failure if reporting is rejected. WorkLimit or cancellation can prevent the report from merging. A `Status` thrown during tensor-window acquisition/read or `std::bad_alloc` caught by the outer math callback can bypass this local report. Cache hits add no new accumulator attempts; Empty demand does no sample arithmetic.

Run the focused test with the configured build directory:

```sh
cmake --build build/kernel-dev --target photospider_numeric_scans -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_scans strict
python3 oracle/ops/numeric/scan_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_scans strict
python3 oracle/ops/numeric/scan_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_scans apple
ctest --test-dir build/kernel-dev -R '^test_numeric_scans_result$' --output-on-failure
```

The installed consumer uses the same source against `Photospider::kernel`:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-repeat-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/result-repeat-install" -DPhotospider_DIR="$PWD/build/kernel-dev/result-repeat-install/lib/cmake/Photospider" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_scans_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_scans_result$' --output-on-failure
```

The final focused root selection completed 40 tests in 19.79 seconds: 39
passed and one skipped, with zero failures. It included this dedicated test
and the shared `test_numeric_result_math` fixture. The Strict and Apple default
workflows exit successfully. The independent scan oracle passes 2,544 cases
per profile. The installed numeric selection passed 10/10. Root resource tests
passed 3/3. Sibling-source 4 MiB and 5 MiB Root Payload peak assertions passed
in the complete `test_dependency_program` run. The skipped test was
`test_vulkan_gpu`, with return code 77. x86 execution, Vulkan execution and new
performance measurements were not run. The installed
consumer compiles against the installed prefix include directory
with `-fno-fast-math`, `-frounding-math` and `-ffp-contract=off`, and links the
installed `libphotospider.a`. This focused check is not a full package matrix.
No x86 execution or performance result is claimed for this Result workflow.
