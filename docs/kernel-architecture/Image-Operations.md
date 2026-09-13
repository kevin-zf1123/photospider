# Float32 Image Operations

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

## S2 Gaussian, mask and composition

The built-in registry and maintained ABI9 C package also provide:

| Operation | Ordered inputs | Required static parameters | Region rule |
| --- | --- | --- | --- |
| `image.gaussian_blur` | RGBA image | `radius:Int64 [1,64]`, `sigma:Float64 [0.1,64]` | Halo resolved from radius; all RGBA channels |
| `image.mask` | RGBA image, Float32 `{H,W}` mask | None | Elementwise; mask maps matching H/W |
| `image.source_over` | Foreground RGBA, background RGBA of identical shape | None | Elementwise; MatchAllInputs |

All operations are CPU, deterministic and side-effect-free. Images preserve
logical shape and the profile above. Masks carry `encode_semantic(coverage_semantics())` and finite samples
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

The example directory is also an independent `find_package(Photospider 0.7)`
consumer. `test_installed_consumer` builds and runs it against isolated static
and shared installations, both with built-ins and with the separately built C
module. Pass the trusted module's exact path as the sole optional argument.

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

## S4 native Metal implementations

Package 0.9 / operation ABI 9 implements all eight operations with the same
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
positive and signed/HDR whole/tiled/nonzero ROI scenes, alpha 0/1/1e-10,
invalid numeric/facet/association inputs, radius 64, factor 16, HDR/subnormal
fallback and exact
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

The independently installable `examples/s4_gpu_workflow` consumer exposes the
same signed scenes through public WorkflowDocument, compile and execute APIs:

```sh
cmake --build build/issue257-static --target photospider_s4_gpu_workflow -j 8
build/issue257-static/examples/s4_gpu_workflow/photospider_s4_gpu_workflow --scenario all-operations --backend cpu
build/issue257-static/examples/s4_gpu_workflow/photospider_s4_gpu_workflow --scenario all-operations --backend metal --require-native
```

Append `--module /absolute/path/to/libphotospider_rgba32f_ops.so` for the C
package. Expected output includes `operations=8 signed_hdr=passed` (with the
`dispatches` field between them) and `oracle=passed`. CPU and eligible native
runs report `fallback_count=0`; native runs must report nonzero dispatches.
Without a device, Metal mode reports the actual positive fallback count;
`--require-native` additionally exits 77. Each scene checks every demanded sample and its
typed facet. For example, signed exposure at `(y=0,x=1)` transforms
`[-2.125,4,-.125,.5]` with gain 2 into `[-4.25,8,-.25,.5]`. In
`image_fixture.hpp`, modify `foreground`, `background`, brush inputs or
`scene()` parameters to compose another experiment, and update the independent
oracle accordingly. Whole execution and the nonzero ROI with 2x3 tiles use the
same mathematical oracle; no negative RGB result is clipped or sent to CPU
solely because of its sign.

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

## Local Navier-Stokes inpaint (PNT-05A)

[`PNT-05A_local_inpaint_navier_stokes.md`](../built-in_ops/09-composite/op_specs/PNT-05A_local_inpaint_navier_stokes.md)
revision 0.3.0 freezes the profile `opencv_4_12_ns_f32_planar_v1`: OpenCV 4.12.0
`INPAINT_NS` applied to three single-channel Float32 planes in R, G, B order.
Two explicit keys are required. The unsuffixed
`image.local_inpaint_navier_stokes` is the semantic family name, not a third
registration or alias.

| Operation | Input 0 | Input 1 | Parameters | Named output |
| --- | --- | --- | --- | --- |
| `image.local_inpaint_navier_stokes_openCV` | Image | Float32 `{H,W}` canonical coverage | required Int64 `radius`, inclusive `[1,32]` | `image`, Float32 `{H,W,4}` |
| `image.local_inpaint_navier_stokes_native_apple_silicon` | Image | same | same | same |

The image port is a typed `SemanticKind::Image` Float32 rank-three constraint,
because a fixed `RgbaFloat32` port cannot express the profile's legal scene or
display reference. The callback validates the exact remaining interpretation:
canonical linear sRGB D65, coverage-premultiplied association, four channels with
their canonical roles, exactly one facet, and a `scene` or `display` reference.
Every other association, primaries, white, transfer, model, channel set or extra
facet fails with `TypeMismatch`, and the published result preserves the input
facet byte-for-byte.

Hole coverage stays the exact canonical `Float32Mask` port. Both inputs must be
finite, alpha must be exactly one and mask samples must be exactly `0` or `1`;
both signed zeros are known samples, and `0.5` coverage is invalid for this
profile even though it is valid coverage. The complete logical domain of both
inputs is validated before the all-zero identity shortcut and before algorithm
entry, so nonfinite RGB placeholders inside holes, nonopaque alpha and nonbinary
coverage fail even for a noop mask. `K=0` returns the unedited input bits,
`K=H*W` fails with `OperationFailed`, a spatial extent below three fails with
`TypeMismatch`, an extent above 32768 on either axis fails with
`ResourceExhausted`, and a mask on a different logical grid is rejected. Only
hole RGB samples are written; unmasked samples and every alpha sample stay
bit-identical, and hole results are never clamped to `[0,1]`.

The Region rule is Whole: `need_image=All([H,W,4])` and
`need_hole_mask=All([H,W])` for every legal nonempty query, a nonzero output
Region is a crop of the same complete computation, and any input edit dirties
the whole output. Radius is not a transitive halo. Both operations are CPU only
with no implicit GPU fallback. The two variants share validation, hole zeroing,
0/255 mask materialization, plane packing, write-back, publication and the
arithmetic-exception contract below.

### Arithmetic-exception contract

A finite published hole sample can hide a nonfinite intermediate, for example an
overflowing `VectorLength(gradI)` on alternating signed `1e30` neighbors. Both
variants clear `FE_INVALID | FE_OVERFLOW | FE_DIVBYZERO` before each channel
solve and test them after it, so such a channel fails with `OperationFailed`
instead of publishing a finite-looking value. The surrounding image scope
restores the caller's complete floating-point environment, so the caller's own
rounding mode and sticky exception flags are unchanged, and a clean invocation
leaves no flag behind. The same contract is what makes the identity and
facet-preservation clauses testable: `floating-state` and the exception-flag
cases are part of the in-tree acceptance test.

### Cancellation cadence

Cancellation is observed at most every 4096 logical samples, counting every RGBA
and mask read, across validation, output copying, mask materialization, plane
packing, hole checking and native guard-grid initialization. Each sample-counted
helper observes cancellation as it starts, the output allocation is observed on
both sides, and a cancelled channel reports `Cancelled` before any dirty
arithmetic state is reported, so helper boundaries and stage transitions are
observation points and no uncounted tail bridges two phases. A direct registry
invocation returns this callback status unchanged, which is why the callback
order matters. Native
initialization counts each guard-grid access of the cross-dilated band
construction and resets the per-channel flag grid in the same checked loop
rather than an unchecked bulk store. The ported frontier observes cancellation
at most every 64 pops or 4096 candidate-loop visits, whichever comes first, plus
around every large allocation, channel transition and publication. A row-modulo
check does not satisfy this bound and is not used. The OpenCV adapter cannot
interrupt the pinned library call; it observes cancellation before packing,
after the last packing chunk and after each channel call, and declares that
limitation instead of claiming the frontier cadence.

### Variant boundaries

The OpenCV adapter packs one Float32 plane per channel, zeroes hole samples and
calls the pinned library once per channel. The library's own guard-grid and
heap-vector allocations are outside the invocation allocator; they are reported
by `estimated_external_bytes` (7 bytes per padded sample for `f`, `band`, `mask`
and `t`, plus up to 32 bytes per padded sample for the inserting-order heap
vector at two-times geometric growth capacity) and are never charged to the host
execution budget. A refused pinned allocation (`cv::Error::StsNoMem`, host
`std::bad_alloc`) maps to `ResourceExhausted`; every other pinned failure maps to
`OperationFailed`.

The native Apple Silicon variant is a licensed standalone port of the pinned
single-channel `icvNSInpaintFMM<float>` frontier, `FastMarching_solve` and
narrow-band construction, with the retained Intel License Agreement notice. It
includes no OpenCV header, symbol or linkage; a native-only build is produced
with `-DPHOTOSPIDER_ENABLE_OPENCV_INPAINT=OFF`. All of its scratch comes from
the invocation allocator: one packed work plane (4N), the internal UInt8 mask
(N), the padded state/band/flags triple (3P), padded arrival times (4P) and a
bounded heap of 16-byte `{float T, int32 y, int32 x, int32 order}` entries
(16P), so `5N+23P` bytes are host-accounted and released on every failure path.

The pinned index branches, guard grid, `1.0e6f` initialization and insertion
order are preserved. `FastMarching_solve` keeps the pinned `double a11, a22,
m12` locals and its binary64 comparisons, mixed term and `1+m12` sums; only the
returned value narrows to Float32. The frontier keeps the pinned binary32
`dst`, gradient, magnitude and `(double)Ia/s` expressions with
round-to-nearest and gradual underflow.

Spec revision 0.3.1 binds one common arm64 OpenCV 4.12.0 reference built with
`-fno-fast-math -frounding-math -ffp-contract=off`, and both variants are
bit-exact to it: zero differing samples over the 5100 compared hole samples of
the acceptance set, and 64/64 on the independent common harness for each
variant, including its `large-frontier` 512x512 random-hole case. Port fidelity
was confirmed directly by compiling the pinned `inpaint.cpp` verbatim into one
diagnostic translation unit with the kernel's flags: over that fixture (65262
hole samples per channel) the verbatim source and the registered native
operation differ in zero samples on all three channels. Each variant also
repeats bitwise for identical invocations.

Historical note: the package-manager `libopencv_photo` distribution is built
with FMA contraction (48 `fmadd` and 18 `fmsub` inside `icvInpaint`). The same
verbatim source compiled with `-ffp-contract=fast` reproduces that distribution
bit-for-bit, while a non-contracted build differs from it by up to 15 times the
frozen tolerance on the 512x512 fixture, because sequential hole dependencies
amplify last-place differences. That measurement is why the profile, the adapter
linkage and the native port all bind the same non-contracted reference instead
of the distribution build; the native translation unit shares the shared
operation flags with no per-file override.

### Evidence

[`test_local_inpaint_navier_stokes.cpp`](../../tests/integration/test_local_inpaint_navier_stokes.cpp)
covers the frozen acceptance matrix, reproduces the coordinator harness fixtures
(block holes at radius 1/3/8/32, extents above the bound, display reference,
nonfinite intermediates, exception-flag restoration) and is rebuilt unchanged as
an installed consumer. The mapping, commands, evidence and current gaps are
recorded in
[`examples/inpaint_ns_workflow/README.md`](../../examples/inpaint_ns_workflow/README.md).
The independent oracle is the pinned library called directly on Float32 planes
with zeroed hole samples and restored unmasked samples; it never calls a
production helper, and it is linked only when `pkg-config opencv4` reports
4.12.0.

```sh
cmake -S . -B build/inpaint-ns -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON -DPHOTOSPIDER_ENABLE_METAL=OFF -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/inpaint-ns --target test_local_inpaint_navier_stokes \
  photospider_inpaint_ns_workflow -j 3
ctest --test-dir build/inpaint-ns \
  -R '^(test_local_inpaint_navier_stokes|example_inpaint_ns_workflow)$' \
  --output-on-failure
```

`-DCMAKE_OSX_ARCHITECTURES=arm64` is required for the pinned profile: the
provisioned OpenCV 4.12.0 build is arm64-only, and a translated x86_64
configure cannot link it. The adapter is disabled with a warning when
`pkg-config opencv4` is missing or reports another version; the native variant
is never affected.
