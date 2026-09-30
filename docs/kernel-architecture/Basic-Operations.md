# Basic operations

The default registry provides CPU operations for curves, fields, scalar statistics, levels, and elementwise numeric arrays. These operations use the public workflow, compiler, and execution APIs. Numeric callback implementations consume `Value` inputs; image ports in this catalog use the legacy value representation and do not imply planar image support. Operation plugins use C ABI 11 and planar extension ABI 3.

## Operation inputs and outputs

| Key | Inputs and output | Required parameters |
| --- | --- | --- |
| `curve.sample_linear`, `curve.sample_monotone` | Generic Float32/Float64 controls `[K,2]` to generic `[count]`; `K >= 2`, all samples finite and control x strictly increasing | Finite Float64 `domain_min < domain_max`; String `out_of_domain` (`reject` or `clip`); Int64 `count` in `[2,1048576]` |
| `field.apply_lut_1d` | Rank-2 field and generic same-dtype rank-1 table `[N]`, `N >= 2`; output preserves field shape; Whole | Finite Float64 `domain_min < domain_max`; String `out_of_domain` (`reject` or `clip`) |
| `field.smoothstep` | Float32/Float64 rank-2 field to Float32 coverage field | Finite Float64 `edge0 < edge1` |
| `analysis.histogram` | Float32/Float64 rank-2 field to Int64 `[bins]` | Int64 `bins` in `[1,1048576]`; finite Float64 `range_min < range_max` |
| `analysis.histogram_out_of_range` | Float32/Float64 rank-2 field to Int64 `[2]` in underflow, overflow order | Finite Float64 `range_min < range_max` |
| `grade.levels` | Float32/Float64 rank-2 field to the same dtype and shape | Finite Float64 `black < white`, `gamma > 0`, `out_min <= out_max` |
| `numeric.minimum`, `numeric.maximum` | Two same-dtype Float32/Float64 generic arrays with identical shapes; same-shape output | None |
| `numeric.abs` | One Float32/Float64 generic array; same-shape output | None |
| `image.mix` | Two RGBA Float32 image Values and one Float32 mask Value; output preserves the first image schema | None |

The registry does not inject parameter defaults. A workflow author supplies every required parameter. Example values such as `count=256`, `domain_min=0`, `domain_max=1`, `edge0=0`, `edge1=1`, `range_min=0`, `range_max=1`, `black=0`, `white=1`, `gamma=1`, `out_min=0` and `out_max=1` are caller choices.

Field inputs are rank-2 Float32/Float64 Values with either no facets, ScalarField semantics, or canonical coverage semantics. Generic controls and LUT tables require empty facets. Numeric binary inputs must match in dtype and shape. The built-in callbacks reject nonfinite samples and unrepresentable intermediate or output values with `OperationFailed`.

## Math and region behavior

Curve samplers place `count` query coordinates across the closed domain, including both endpoints. Linear interpolation passes through each control point. Monotone interpolation uses a shape-preserving cubic Hermite curve with weighted harmonic interior slopes and limited endpoint slopes; two controls produce a linear segment. `reject` requires the requested domain to lie within the control x range; `clip` returns the nearest endpoint outside it. Collapsed query coordinates fail.

`field.apply_lut_1d` maps the closed input domain to the table's first and last entries and linearly interpolates between table samples. The String policy rejects out-of-domain field values or clips them to endpoint values. This table input is an ordinary generic array; it does not carry the SampledSignal contract consumed by `lut.apply_1d`.

`field.smoothstep` clamps the normalized edge coordinate to `[0,1]` and evaluates `t*t*(3-2*t)`. `grade.levels` maps `[black,white]` to `[out_min,out_max]`; `gamma=1` uses compensated linear interpolation, while other positive gamma values apply `pow(t,1/gamma)` to the clamped coordinate. Levels preserves the input dtype. Smoothstep establishes canonical Float32 coverage semantics.

The histogram divides `[range_min,range_max]` into uniformly spaced bins. Bins are left-closed and right-open, except that the last bin includes the upper endpoint. Values outside the range are omitted from `analysis.histogram` and counted by `analysis.histogram_out_of_range`. The implementation constructs endpoint-exact Float64 edges, rejects collapsed edges, locates values by binary search, checks Int64 counter overflow, and scans the complete field.

`numeric.minimum` chooses negative zero when both operands are zero; `numeric.maximum` chooses positive zero. `numeric.abs` maps negative zero to positive zero. These operations use Elementwise regions. Curves and both histogram operations use Whole regions; LUT application reads its full table and processes the field output; levels and smoothstep process requested output regions. The `image.mix` Value callback computes `(1-M)A+MB` per RGBA sample and uses an Elementwise region, but it does not declare planar storage capability.

## Execution and errors

Callbacks allocate outputs and scratch through the invocation allocator. They poll cancellation during traversal, restore the caller's floating-point environment, and release unpublished allocations on failure. PCHIP keeps three Float64 workspace arrays per control point. Whole operations require their complete declared support to fit the execution resources.

Invalid static parameters return `InvalidArgument`. Incompatible dtype, shape, facets, or image metadata return `TypeMismatch`. Nonfinite samples, collapsed numerical coordinates, counter overflow, or unrepresentable results return `OperationFailed`. Cancellation and resource exhaustion retain their own status codes. Failed callbacks do not publish partial Values.

The legacy `image.mix` registration describes a callback over RGBA `Value` inputs, but a workflow declaration carrying the `photospider.image` facet without `PlanarImageLayout` is rejected during input validation. Its `OperationTraits::planar_storage_capable` flag also remains false, so the compiler rejects a structural planar image input. The registration alone is not evidence of an executable workflow path.

## Public workflows and checks

[`examples/foundations_workflow`](../../examples/foundations_workflow/README.md) provides the maintained generic numeric and expression/LUT examples. [`test_basic_operations.cpp`](../../tests/integration/test_basic_operations.cpp) covers curves, fields, histograms, regional output, views, parameter and numeric failures, cancellation, resource limits, and floating-point environment restoration. It does not exercise `image.mix`.

```sh
cmake --build build --target test_basic_operations photospider_foundations_workflow -j 8
ctest --test-dir build -R '^(test_basic_operations|test_workflow_numeric_reductions|test_workflow_expression_lut)$' --output-on-failure
```
