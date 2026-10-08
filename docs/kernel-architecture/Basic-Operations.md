# Basic operations

The default registry provides CPU operations for curves, fields, scalar statistics, levels, elementwise numeric arrays, and images. Every operation listed on this page uses Result inputs and outputs; its input schema and region contract are defined per key. The foundations workflow also demonstrates the unsuffixed expression and LUT operations through Result. The Result operation C ABI 2 is the only operation-plugin C table; see [Plugin ABI](Plugin-ABI.md).

## Operation inputs and outputs

| Key | Inputs and output | Required parameters |
| --- | --- | --- |
| `curve.sample_linear`, `curve.sample_monotone` | One Result containing generic Float32/Float64 controls `[K,2]` to `photospider.tensor` v1/member `samples`, `[count]`; unbatched, `K >= 2`, finite controls with strictly increasing x; Whole | Finite Float64 `domain_min < domain_max`; String `out_of_domain` (`reject` or `clip`); Int64 `count` in `[2,1048576]` |
| `field.apply_lut_1d` | Field Result and same-dtype generic table Result `[N]`, `2 <= N <= 2^53`; output `photospider.tensor` v1/member `samples` with field sample shape; Whole | Finite Float64 `domain_min < domain_max`; String `out_of_domain` (`reject` or `clip`) |
| `field.smoothstep` | Field Result to canonical Float32 `photospider.image` v1/member `pixels`, sample shape `[1,1,H,W]`; Dependency | Finite Float64 `edge0 < edge1` |
| `analysis.histogram` | Float32/Float64 field Result to generic Int64 `photospider.tensor` v1/member `samples`, `[bins]`; Whole | Int64 `bins` in `[1,1048576]`; finite Float64 `range_min < range_max` |
| `analysis.histogram_out_of_range` | Float32/Float64 field Result to generic Int64 `[2]`, underflow then overflow; Whole | Finite Float64 `range_min < range_max` |
| `grade.levels` | Float32/Float64 field Result to generic `photospider.tensor` v1/member `samples`, preserving complete sample shape and dtype; Dependency | Finite Float64 `black < white`, `gamma > 0`, `out_min <= out_max` |
| `core.delay` | One tensor Result; nonempty requests preserve its schema and produce complete coverage, while Empty returns no tensor coverage; Whole | Required Int64 `milliseconds` in `[0,5000]`; disables optional local cache retention |
| `core.gpu_fallback_probe` | One tensor Result to an identity Result over the requested footprint; dependency execution | None; execution probe for startup-only CPU fallback |
| `numeric.minimum`, `numeric.maximum` | Two same-dtype Float32/Float64 tensor Results with identical `sample_shape()`; output port `value` is a `photospider.tensor` v1 Result with member `samples` | None |
| `numeric.abs` | One Float32/Float64 tensor Result; output port `value` is a `photospider.tensor` v1 Result with member `samples`, same sample shape and dtype | None |
| `image.mix` | Two RGBA image Results and one coverage image Result; output port `value` preserves the first image Result schema | None |

The registry does not inject parameter defaults. A workflow author supplies every required parameter. Example values such as `count=256`, `domain_min=0`, `domain_max=1`, `edge0=0`, `edge1=1`, `range_min=0`, `range_max=1`, `black=0`, `white=1`, `gamma=1`, `out_min=0` and `out_max=1` are caller choices.

Each of the seven operations above accepts Result inputs containing exactly one tensor and no fields. Schema id, version, and tensor key may be any structurally valid values. Inputs use Float32 or Float64. Curve controls are unbatched `[K,2]` with empty facets. A field has rank-two cell shape; its `batch_axes` are empty or `[1,1]`, and its facets are empty, ScalarField, or canonical coverage. The LUT table is unbatched and facet-free, and must match the field dtype. External output port is `value` for every operation. Outputs other than smoothstep use the generic `photospider.tensor` v1 schema with member `samples` and do not copy input facets; smoothstep restores the image coverage Result schema.

Curve sampling, LUT application, and both histograms use Whole execution. A nonempty request reads complete input tensors with Data, Validation, and Descriptor roles; Empty requests do not request payload. Levels and smoothstep use staged Dependency execution: Data is read for Q, while Validation covers `close_samples(Q)` for atomic sample groups. The program can combine these roles when the closure equals Q or issue separate Validation and Data Needs when it expands. Relations retain those supports separately. All seven publish through Result transactions, preserve actual source association, charge Root work, and check cancellation while reading and computing.

`image.mix` accepts `photospider.image` v1 Results with one `pixels` tensor: two canonical Float32 HWC RGBA images and a canonical Float32 HW coverage mask. All three inputs must agree on frame, layer, height, and width extents. The output uses the first image schema. Image operations in `image_program.cpp` read and publish Result tensor windows; `PlanarImage` may provide the physical backing within that Result.

`core.delay` accepts a structurally valid Result with one tensor and no fields, preserves its schema and metadata, and waits the requested milliseconds when a new nonempty continuation starts. It checks cancellation and consumes one work unit per millisecond; Empty output does not request input payload. `cacheable=false` disables optional local cache retention, but a request with the same frozen snapshot, operation contract, and Q can still join or reuse a completed shared Result while an external owner keeps it alive. The delay therefore does not imply that every execution waits again. `core.gpu_fallback_probe` maps the selected footprint as an identity and permits CPU fallback. It is an execution probe: its GPU `start_result` returns `BackendUnavailable`, allowing the coordinator to exercise a CPU retry before any Need or native dispatch.

## Math and region behavior

Curve samplers place `count` query coordinates across the closed domain, including both endpoints. Linear interpolation passes through each control point. Monotone interpolation uses a shape-preserving cubic Hermite curve with weighted harmonic interior slopes and limited endpoint slopes; two controls produce a linear segment. `reject` requires the requested domain to lie within the control x range; `clip` returns the nearest endpoint outside it. Collapsed query coordinates fail.

`field.apply_lut_1d` maps the closed input domain to the table's first and last entries and linearly interpolates between table samples. The String policy rejects out-of-domain field values or clips them to endpoint values. This table input is an ordinary generic array; it does not carry the SampledSignal contract consumed by `lut.apply_1d`.

`field.smoothstep` clamps the normalized edge coordinate to `[0,1]` and evaluates `t*t*(3-2*t)`. `grade.levels` maps `[black,white]` to `[out_min,out_max]`; `gamma=1` uses compensated linear interpolation, while other positive gamma values apply `pow(t,1/gamma)` to the clamped coordinate. Levels preserves the input dtype. Smoothstep establishes canonical Float32 coverage semantics.

The histogram divides `[range_min,range_max]` into uniformly spaced bins. Bins are left-closed and right-open, except that the last bin includes the upper endpoint. Values outside the range are omitted from `analysis.histogram` and counted by `analysis.histogram_out_of_range`. The implementation constructs endpoint-exact Float64 edges, rejects collapsed edges, locates values by binary search, checks Int64 counter overflow, and scans the complete field.

`numeric.minimum` chooses negative zero when both operands are zero; `numeric.maximum` chooses positive zero. `numeric.abs` maps negative zero to positive zero. These CPU operations use staged Dependency execution: Data is read only over the selected output footprint Q, while Validation covers `close_samples(Q)` for typed tuples and atomic sample groups. Validation can inspect tuple samples outside Q when closure requires them, and it does not request samples beyond `close_samples(Q)`. If that closure equals Q, the operator combines Data and Validation roles into one Need; otherwise, it sends a Validation Need and then a Data Need. The output relation keeps Data, Validation, and Descriptor support distinct. For `image.mix`, the image program linearly blends all four RGBA channels with the coverage mask over the requested image regions. Curves and both histogram operations use Whole regions; LUT application reads its full table and processes the field output; levels and smoothstep process requested output regions.

## Execution and errors

These Result programs read authorized tensor windows and publish through the Result builder. They poll cancellation and restore the caller's floating-point environment; failed operations discard unpublished output. PCHIP keeps three Float64 workspace arrays per control point. Whole operations require their complete declared support to fit the execution resources.

Invalid static parameters return `InvalidArgument`. Incompatible dtype, shape, facets, or image metadata return `TypeMismatch`. Nonfinite arithmetic samples, collapsed numerical coordinates, counter overflow, or unrepresentable results return `OperationFailed`. Typed Result source validation follows its input error contract and can return `ErrorCode::InvalidArgument` with `FailureReason::InvalidDomain`, preserving the source `input_id`. Cancellation and resource exhaustion retain their own status codes. These Result operations publish only after their Result transaction succeeds.

The image Result contract is carried by `photospider.image` v1, one `pixels` tensor, canonical RGBA or coverage semantics, and the layout declared by its tensor spec. It does not use the legacy `Value` planar capability flag.

## Public workflows and checks

[`examples/numeric_workflow/README.md#basic-curves-fields-and-analysis-results`](../../examples/numeric_workflow/README.md#basic-curves-fields-and-analysis-results) shows the Result curve, field, level, histogram, and smoothstep-to-mask workflow. [`examples/foundations_workflow`](../../examples/foundations_workflow/README.md) demonstrates Result-based numeric, expression/LUT, and generator-gain scenarios. [`test_basic_operations.cpp`](../../tests/integration/test_basic_operations.cpp) covers the basic Result contracts alongside remaining field and image behaviors; [`test_numeric_result_math.cpp`](../../tests/integration/test_numeric_result_math.cpp) exercises finite elementwise Result keys; [`test_builtin_result_images.cpp`](../../tests/integration/test_builtin_result_images.cpp) exercises Result image programs including `image.mix`.

```sh
cmake --build build/kernel-dev --target test_basic_operations photospider_numeric_basic -j8
ctest --test-dir build/kernel-dev -R '^test_basic_operations$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_basic
```
