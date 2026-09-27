# Retired Float32 image operation contract

Package 0.19 retires the packed image execution contract described below.
These declarations, numerical rules and S1–S4 commands are migration reference,
not the current executable image API. Image operations require explicit planar
storage capability; unsupported legacy calls fail without compatibility fallback.
Use [Tensor storage and region access](../kernel-specs/Tensor-Storage-and-Region-Access.md)
for the current CPU interfaces and runnable workflow. Generic scalar and
non-image tensor facilities remain supported under their own contracts.

The default registry includes CPU operations implemented in
[`plugins/ops/README.md`](../../plugins/ops/README.md).
The two S1 operations below have two ordered runtime Value inputs, no compile-time parameters or
implicit defaults, and one regional image output named by the workflow.

| Operation | Input 0 | Input 1 | Output |
| --- | --- | --- | --- |
| `image.exposure_gain` | Image | Float32 scalar gain, inclusive [0,16] | RGB multiplied by gain; alpha copied bit-for-bit |
| `image.opacity` | Image | Float32 scalar opacity, inclusive [0,1] | All RGBA channels multiplied by opacity |

An image declaration is dense Float32 {H,W,4}, H/W positive, whole Region, offset
zero and canonical row-major strides. Runtime views have explicit origin, strides
and valid Region. Both carry exactly one facet: key `photospider.image`,
version 2, the canonical payload from `encode_semantic(rgba_semantics())`.
Image-v1 metadata is rejected; callers use the public typed helper.
RGB is finite and signed; alpha is finite in [0,1], and alpha zero requires
RGB zero. HDR RGB may exceed one or alpha. Signed zero is accepted. The caller
supplies linear sRGB/Rec.709, D65, scene-referred relative RGB with dimensionless
coverage alpha and coverage-premultiplied association; no color conversion, gamma,
clamp or unpremultiplication occurs.

Scalar inputs may be direct workflow declarations or upstream Float32 `{1}`
results. Allowed facets are none, one dimensionless Scalar, or one dimensionless
single-sample SampledSignal. The sampling-axis unit/domain is independent of the
sample value unit and remains intact. Other typed or opaque facets are rejected.
Direct bindings retain whole dense declarations (offset zero, stride `{4}`, four
bytes). Computed views require complete `{1}` coverage and may be padded,
unaligned, broadcast or negatively strided; C++, C and Metal marshalling read
logical sample zero with byte-safe access. No cast or clamp occurs. Per-Run
scalar bytes do not change compiled-plan identity; eligible result keys include
both the bytes and allowed semantic facets.

These operations are deterministic, side-effect-free, cacheable, PreserveFirstInput
and Elementwise. Image input demand equals requested spatial output demand with
all four channels; scalar demand is always whole {1}. Smaller demand returns only the requested Region, preserving the logical descriptor. Each image step reserves its output bytes through the host allocator; no
second sink copy is needed. The complete Run reservation includes retained
intermediates and scratch. Caller-preexisting inputs and process RSS are
outside the controlled-buffer bound.

Multiplication rounds each stage to IEEE binary32 nearest, ties to even, with
gradual underflow. Host schema/numeric validation and image callback scopes
save and restore the thread's floating environment, preventing inherited
rounding or flush-to-zero modes from changing the result. Computed non-finite
pixels, invalid profile or alpha-zero/nonzero-RGB output fail OperationFailed.
Direct scalar errors fail InvalidArgument before work; invalid computed scalar
numbers fail OperationFailed before each consuming callback, including cached
and shared-producer results. Metadata mismatches are TypeMismatch. Pixel errors
fail before the consuming callback. Unread pixels are not scanned.

All eight operations implement this image-v2 contract in C++, C and Metal.
Each declares PreserveInput semantics and publishes the first input's exact facet;
box operations change only the logical H/W. Their ports require canonical RGBA
or typed coverage masks. Straight alpha, RGB-only, reordered channels and other
color models require explicit conversion before these operations.

## Reusable operation package and executable example

[`plugins/ops/rgba32f`](../../plugins/ops/rgba32f/CMakeLists.txt) builds the
maintained ABI9 C module `photospider_rgba32f_ops` using only
`Photospider::operation_sdk`. It implements the same image operations and profile
as the built-ins above, with strict floating-point compilation. The ABI9 host
validates ports and establishes nearest/gradual-underflow arithmetic before
entry. The callback requests its output from the host allocator and publishes that
same buffer; success freezes it, and failure releases it without publication. Load this
trusted package into an empty registry and freeze it before compilation;
its operation keys are already present in the default registry.

[`examples/image_vertical/image_fixture.hpp`](../../examples/image_vertical/image_fixture.hpp)
is the shared public fixture contract for tests, installed consumers, and the
companion daemon vertical. It fixes the exact declarations, chain, A/B values,
shape/layout/facets, requested pixel (0,1), and output table from
[ADR 0016](../adr/0016-workflow-inputs-and-execution-bindings.md#named-fixtures-and-image-oracle).
The bounded CPU oracle `s1-rgba32f-exposure-opacity-v1` independently computes
16 channels from each binding snapshot, rounds each stage to binary32, checks
that calculation against the frozen table, and compares the complete named
`result` logical descriptor and the requested pixel's 16 bytes exactly. It calls no operation callback.

[`photospider_image_vertical`](../../examples/image_vertical/main.cpp) compiles
once and executes A/B with that same plan. Each run requires two successful CPU
callbacks (nodes 10,20), unchanged plan identity, the expected distinct result
digests, zero transfers/bytes/fallbacks, and peak 48 actual allocated bytes. Both image
input demands and step output demands are offsets {0,1,0}/extents {1,1,4};
scalar demand stays whole {1}, and the result remains the one-pixel Region with logical shape {2,2,4}.
The executable prints named input/output Values, descriptor/Region/layout/facets,
plan/result digests, compile/execute/operation timings, selected backends,
transfer/resource observations, and correctness on separate lines. Timing
values may be zero. Digests are diagnostic; correctness uses actual bytes.

It then runs two raw benchmark samples per payload with the matching captured
CPU oracle. These samples retain `RawBenchmarkRunner`'s independent compilation
semantics and are reported separately from the compile-once executions.
A mismatch exits nonzero. With no argument it uses built-ins; its optional
argument is the exact trusted native module path.

```sh
cmake --build build/issue257-static --target photospider_image_vertical test_bindings -j 8
build/issue257-static/examples/image_vertical/photospider_image_vertical
ctest --test-dir build/issue257-static -R '^test_(image_vertical|image_vertical_plugin|bindings|installed_consumer)$' --output-on-failure
```

`test_image_vertical_plugin` passes the generator-resolved package path to the
same executable. `test_bindings` retains the exact negative binding and output
demand cases, independent concurrent snapshots, numeric/floating-environment
boundaries, cancellation, and resource checks; its positive DSO path now uses
the maintained package. The intentionally invalid output DSO stays test-only.

For an installed kernel prefix, both source directories also build independently:

```sh
cmake -S plugins/ops/rgba32f -B build/rgba32f-package -DCMAKE_PREFIX_PATH=/absolute/kernel-prefix -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/rgba32f-package --target photospider_rgba32f_ops -j 8
cmake -S examples/image_vertical -B build/image-example -DCMAKE_PREFIX_PATH=/absolute/kernel-prefix -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/image-example --target photospider_image_vertical -j 8
build/image-example/photospider_image_vertical /absolute/path/to/native-module
```

The isolated installed consumer builds this same operation source package
against the installed SDK, runs A/B through its shared bridge, and runs the
same executable with built-ins and the module. Static and shared kernel builds
exercise this path and package 0.7/rejected 0.6 requests; see
[Testing and Validation](../development/Testing-and-Validation.md).

## S2 mask and composition

The default registry retains `image.mask` and `image.source_over`. The former
accepts an RGBA image and a matching Float32 `{H,W}` coverage mask; each finite
mask sample in `[0,1]` multiplies all foreground channels. Source-over accepts
matching foreground and background images and computes `F + B * (1 - F.alpha)`
per premultiplied channel. Both are elementwise CPU operations. The former
built-in `image.gaussian_blur` has been removed; proposed replacement behavior
is in [05-filter](../built-in_ops/05-filter/spatial.md).

## S3 box shrink and circle stamp

Package 0.9 / operation ABI 9 exposes the following built-ins and the same C
module operations. These use existing Float32 linear-sRGB premultiplied RGBA
and finite [0,1] Float32 HW masks. All parameters listed as scalar inputs are
ordinary Float32 `{1}` bindings, not compile-time node parameters.

| Name | Inputs | Static parameters | Output and Region |
| --- | --- | --- | --- |
| `image.downsample_box` | RGBA image | Required Int64 `factor` in [1,16], no implicit default | RGBA `{ceil(H/f),ceil(W/f),4}`; clipped integer-box demand |
| `mask.downsample_box` | HW mask | Same `factor` | Mask `{ceil(H/f),ceil(W/f)}`; clipped integer-box demand |
| `image.brush_circle` | image, x, y, radius, red, green, blue, alpha, in this order | None | Same image shape; Elementwise demand |

Box operations sum each cell in binary64 row/column order and round the actual
covered-sample average to binary32. Edges divide by their actual sample count.
Factor one preserves numeric values. No gamma conversion or unpremultiplication
occurs. The application preview defaults to factor four.

Brush x/y accept all finite Float32; radius accepts positive normal Float32
through FLT_MAX; linear unassociated RGB accepts [-FLT_MAX,FLT_MAX], alpha [0,1]. Every
input is required. The closed circle tests pixel centers using binary64 squared
distance. Inside, source RGB is multiplied by alpha in binary32 and composited
with the premultiplied background using source-over without contraction;
outside, all sample bits are retained. One event is one hard-edge circle, with
no antialiasing, interpolation, pressure or device input. The application plans
the clipped bounding ROI and applies its result as a snapshot patch.

`test_s3_operations [trusted-module]` runs public compile/execute examples with
independent box-distribution and circle oracles, including odd sizes, edge ROIs,
factors 1/2/4/16 and invalid scalar inputs. The reusable interactive example is
tracked by #275/#277.

## S4 native Metal boundary

The independently built [`rgba32f` operation module](../../plugins/ops/rgba32f)
has its own C ABI registration and shader package. It is separate from the
default repository-owned built-in registry. Removing the built-in 05-filter
implementation does not register a replacement filter or establish native
support for any proposed FIL member. Current default-registry image and Metal
behavior must be checked against the corresponding registered operation and
planar execution tests.

## Computed scalar composition

[`test_computed_scalar.cpp`](../../tests/integration/test_computed_scalar.cpp)
registers a small public `coefficient.scale` producer and connects its result to
exposure, opacity or brush through WorkflowDocument. One compiled plan changes
coefficient bindings between sequential/concurrent Runs. For exposure, coefficient
1 generates gain 2; coefficient 3 generates gain 6, so the same source pixel's RGB
triples while alpha stays unchanged. A cached value 1.5 is legal as gain and
rejected as opacity. Generic NaN results remain valid standalone Values but cannot
enter either bounded consumer. The fixture demonstrates Scalar/Signal metadata,
five layouts, field/opaque rejection and independent shared cancellation.

```sh
cmake --build build/issue257-static --target test_computed_scalar -j 8
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --test-dir build/issue257-static -R '^test_computed_scalar' --output-on-failure
```

Both C++ and C consumer runs report `layouts=5 semantic_kinds=3` and
`oracle=passed`; available native hardware must execute 45 dispatches. Without
native hardware the same test verifies CPU/fallback behavior and reports zero
native dispatches. Change the fixture's coefficient binding or the pure producer
callback to continue composing; expression parsing is a later operation slice.
