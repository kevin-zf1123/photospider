# Float32 Image Operations

The default registry includes two CPU operations implemented in
[`plugins/ops/image_operations.cpp`](../../plugins/ops/image_operations.cpp).
Both have two ordered runtime Value inputs, no compile-time parameters or
implicit defaults, and one complete image output named by the workflow.

| Operation | Input 0 | Input 1 | Output |
| --- | --- | --- | --- |
| `image.exposure_gain` | Image | Float32 scalar gain, inclusive [0,16] | RGB multiplied by gain; alpha copied bit-for-bit |
| `image.opacity` | Image | Float32 scalar opacity, inclusive [0,1] | All RGBA channels multiplied by opacity |

An image is dense Float32 {H,W,4}, H/W positive, whole Region, offset zero,
canonical row-major strides, and exactly one facet: key `photospider.image`,
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
all four channels; scalar demand is always whole {1}. Smaller demand still
returns the complete dense image. Each image step models at least twice its
output byte count for callback output and host copy. This is not a total
input/intermediate/process memory bound.

Multiplication rounds each stage to IEEE binary32 nearest, ties to even, with
gradual underflow. Host schema/numeric validation and image callback scopes
save and restore the thread's floating environment, preventing inherited
rounding or flush-to-zero modes from changing the result. Computed non-finite
pixels, invalid profile or alpha-zero/nonzero-RGB output fail OperationFailed.
Bound pixel/scalar domain errors fail InvalidArgument before any callback.

## Executable public-API example

[`tests/consumer/image_fixture.hpp`](../../tests/consumer/image_fixture.hpp)
constructs the exact three declarations, two nodes and A/B binding snapshots
from [ADR 0016](../adr/0016-workflow-inputs-and-execution-bindings.md).
[`test_bindings.cpp`](../../tests/integration/test_bindings.cpp) compiles once,
executes both snapshots sequentially and concurrently, and compares the named
`result` descriptor and all 64 output bytes with
`s1-rgba32f-exposure-opacity-v1`. It also covers changing each input separately,
opacity zero, failure preflight, schemas, Halo, stopping and resource bounds.

```sh
cmake --build build/issue257-static --target test_bindings -j 8
ctest --test-dir build/issue257-static -R '^test_bindings$' --output-on-failure
```

The isolated installed consumer runs the same oracle through both the default
C++ operations and its independently compiled ABI3 C plugin. It checks package
0.3 compatibility and rejection of a 0.2 consumer. Build with BUILD_SHARED_LIBS
OFF and ON to exercise both package forms; see
[Testing and Validation](../development/Testing-and-Validation.md).
