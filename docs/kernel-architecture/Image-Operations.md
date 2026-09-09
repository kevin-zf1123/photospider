# Float32 Image Operations

The default registry includes CPU operations implemented in
[`plugins/ops/image_operations.cpp`](../../plugins/ops/image_operations.cpp).
The two S1 operations below have two ordered runtime Value inputs, no compile-time parameters or
implicit defaults, and one regional image output named by the workflow.

| Operation | Input 0 | Input 1 | Output |
| --- | --- | --- | --- |
| `image.exposure_gain` | Image | Float32 scalar gain, inclusive [0,16] | RGB multiplied by gain; alpha copied bit-for-bit |
| `image.opacity` | Image | Float32 scalar opacity, inclusive [0,1] | All RGBA channels multiplied by opacity |

An image declaration is dense Float32 {H,W,4}, H/W positive, whole Region, offset
zero and canonical row-major strides. Runtime views have explicit origin, strides
and valid Region. Both carry exactly one facet: key `photospider.image`,
version 1, payload `rgba;linear-srgb;premultiplied;hwc` (34 ASCII bytes, no NUL).
RGB is finite and nonnegative; alpha is finite in [0,1], and alpha zero requires
RGB zero. HDR RGB may exceed one or alpha. Signed zero is accepted. The caller
supplies already linear-sRGB premultiplied values; no color conversion, gamma,
clamp or unpremultiplication occurs.

Scalar inputs are direct workflow declarations with Float32 {1}, whole Region,
offset zero, stride {4}, four bytes and no facets. Per-run gain/opacity bytes
are not source parameters and do not change compiler identities.

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
Bound scalar errors fail InvalidArgument before work; pixel errors fail before
the consuming callback. Unread pixels are not scanned.

## Reusable operation package and executable example

[`plugins/ops/rgba32f`](../../plugins/ops/rgba32f/CMakeLists.txt) builds the
maintained ABI6 C module `photospider_rgba32f_ops` using only
`Photospider::operation_sdk`. It implements the same image operations and profile
as the built-ins above, with strict floating-point compilation. The ABI6 host
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
exercise this path and package 0.6/rejected 0.5 requests; see
[Testing and Validation](../development/Testing-and-Validation.md).

## S2 Gaussian, mask and composition

The built-in registry and maintained ABI6 C package also provide:

| Operation | Ordered inputs | Required static parameters | Region rule |
| --- | --- | --- | --- |
| `image.gaussian_blur` | RGBA image | `radius:Int64 [1,64]`, `sigma:Float64 [0.1,64]` | Halo resolved from radius; all RGBA channels |
| `image.mask` | RGBA image, Float32 `{H,W}` mask | None | Elementwise; mask maps matching H/W |
| `image.source_over` | Foreground RGBA, background RGBA of identical shape | None | Elementwise; MatchAllInputs |

All operations are CPU, deterministic and side-effect-free. Images preserve
logical shape and the profile above. Masks have no facets and finite samples
in `[0,1]`; each mask sample multiplies all foreground RGBA channels. Source-over
computes `F + B * (1 - F.alpha)` separately for each channel using premultiplied
values, following the [W3C formula](https://www.w3.org/TR/compositing-1/#porterduffcompositingoperators_srcover).
The subtraction, multiplication and addition round to Float32 with no FMA.

Gaussian computes normalized binary64 `exp(-tap²/(2*sigma²))` weights in tap order
`-radius..radius`. Each horizontal and then vertical pass accumulates binary64
products in that order and rounds its output to Float32. Edges clamp at the
full logical image boundary; tile edges never clamp independently. Radius and
sigma are source parameters and changes require recompilation. Workspace bounds
are 1032 fixed coefficient bytes plus one byte per demanded input byte; the
horizontal scratch only stores demanded rows and output columns. All coefficient,
scratch and output buffers come from the host allocator. Fast-math and FMA
contraction are disabled for both C++ and C implementations.

[`photospider_regional_image_vertical`](../../examples/regional_image_vertical/main.cpp)
runs `foreground -> Gaussian -> exposure -> mask -> source-over(background)`
through public compile/execute/execute_stream. Fixture `S2Image.RegionAndTiles`
checks a hand-computed uniform scene (RGB .3125, alpha .625), a separate full-image
2D Gaussian oracle (`atol=1e-6, rtol=1e-5`), and bitwise equality between whole and
1x1/2x3/5x7/128x128 tiles. It includes a nonzero ROI, edges, non-divisible tiles,
radius 64, sigma .1, transparent/HDR values, mask 0/1, dynamic gain reuse and
invalid parameter/mask/shape cases. The independent oracle shares no operation
callback or separable-pass implementation.

A 65536x65536 procedural source variant streams a 5x7 ROI in nine tiles, checks
sample values, 9900 source bytes and an actual peak of 1808 bytes, and tests its
3840-byte conservative reservation
exactly and one byte short. This proves a controlled
buffer bound, not a process RSS bound. Regional-source, fan-out, concurrent-Run,
cancellation, stale and sink-failure coverage is in `test_regional_execution`
and `test_memory_liveness`.

```sh
cmake --build build/issue257-static --target photospider_regional_image_vertical -j 8
build/issue257-static/examples/regional_image_vertical/photospider_regional_image_vertical
ctest --test-dir build/issue257-static -R '^test_(s2_vertical|s2_vertical_plugin|regional_execution|installed_consumer)$' --output-on-failure
```

The example directory is also an independent `find_package(Photospider 0.6)`
consumer. `test_installed_consumer` builds and runs it against isolated static
and shared installations, both with built-ins and with the separately built C
module. Pass the trusted module's exact path as the sole optional argument.

## S3 box shrink and circle stamp

Package 0.6 / operation ABI 6 exposes the following built-ins and the same C
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
through FLT_MAX; linear unassociated RGB accepts [0,FLT_MAX], alpha [0,1]. Every
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

## S4 native Metal implementations

Package 0.6 / operation ABI 6 implements all eight operations with the same
trusted pure C host GPU service. The built-in adapter and independently built
C11 module share the maintained `plugins/ops/rgba32f/image.metal` program and
host marshalling. CMake embeds shader text in a generated build header; installed
consumers need no source-tree shader path or Objective-C++ toolchain settings.

`PlanningOptions::execution_mode` defaults to `ExecutionMode::CpuExact`.
`MetalFp32` explicitly permits the approximate native implementation;
`ExecutionContextConfig::gpu_enabled=true` attempts actual Apple Silicon device
creation. Unsupported builds/hardware retain per-operation CPU fallback.

The native domain is conservative: image/mask samples must be zero or have
magnitude at least 1e-20 and at most FLT_MAX/1024; mask multipliers, ordinary
gain/opacity, and brush color/alpha use a nonzero minimum of 1e-8. Positive
Gaussian coefficients below 1e-8 also select CPU. Logical spatial dimensions
must fit uint32; native views need nonnegative strides and four-byte-aligned
byte offsets/strides. Storage origins must not exceed demand offsets on any
axis (the image channel origin is zero). Other legal native predecessors fall
back per invocation.
These restrictions only select an implementation; legal values
outside them retain the full CPU contract. Malformed inputs remain errors.
Safe Metal math and no contraction, compensated sums and host double weights
are checked against independent per-operation and representative-chain oracles
with atol=1e-6 and rtol=1e-5. This is not CPU bit identity or a graph-size-independent
error bound. Circle coverage uses host double row spans, including large logical
coordinates; GPU color computation preserves outside pixel bits.

`test_metal_images` and `test_metal_images_plugin` cover all eight operations,
whole/tiled/nonzero ROI, radius 64, factor 16, HDR/subnormal fallback and exact
large-coordinate stamp coverage. The public fixture is
[`examples/s4_gpu_workflow/image_fixture.hpp`](../../examples/s4_gpu_workflow/image_fixture.hpp).
Use Xcode's command-line validation on actual hardware:

```sh
cmake --build build/issue257-static --target test_metal_images -j 8
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --test-dir build/issue257-static \
  -R '^test_metal_images' --output-on-failure
```

Unavailable hardware returns an explicit CTest skip; it is not a successful
native execution result. The image CPU implementations and prior numerical
contracts remain the exact default.
