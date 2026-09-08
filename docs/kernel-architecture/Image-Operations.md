# Float32 Image Operations

The default registry includes two CPU operations implemented in
[`plugins/ops/image_operations.cpp`](../../plugins/ops/image_operations.cpp).
Both have two ordered runtime Value inputs, no compile-time parameters or
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
maintained ABI4 C module `photospider_rgba32f_ops` using only
`Photospider::operation_sdk`. It implements the same two operations and profile
as the built-ins above, with strict floating-point compilation. The ABI4 host
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
exercise this path and package 0.4/rejected 0.3 requests; see
[Testing and Validation](../development/Testing-and-Validation.md).
