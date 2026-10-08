# Repository-owned operations

`plugins/ops/` contains operation implementations compiled into the kernel and
optional operation packages built against the installed kernel SDK. Each
registered C++ operation keeps its callback and schema near its source file.
`src/lib/plugin/builtin_operations.cpp` aggregates built-in registration
functions; it does not contain operation callbacks. Private algorithms and
addressing helpers remain in internal headers.

| Directory | Responsibility |
| --- | --- |
| `00-foundation` | Core execution probes and image Result runtime, including native dispatch helpers |
| `01-numeric` | Array arithmetic, curves, expressions, LUTs and smoothstep |
| `02-format-color` | Alpha editing, numeric conversion, transfer, RGB basis and model conversion, channel assembly/editing and metadata assignment |
| `03-generation` | Coordinate and constant fields |
| `05-filter` | Gaussian blur built-in; broader spatial-filter contracts are documented separately |
| `07-grade` | Exposure gain and levels |
| `08-transform` | Image and mask downsampling |
| `09-composite` | Opacity, masks, image mix, source-over and brush stamping |
| `10-analysis` | Histograms and out-of-range counts |

Root CMake lists the built-in operation sources in
`PHOTOSPIDER_OPERATION_SOURCES` and uses that list for the product and test
kernels, applying strict floating-point compile options to those sources and
their associated math implementations. The default C++ registry in
`src/lib/plugin/builtin_operations.cpp` registers the current `02-format-color`
operations and Gaussian blur alongside the other built-in families. Changes to
private helper headers also participate in cache build identity through the
recursive source inventory.

`rgba32f/` supplies a private Metal shader helper for the shared image Result
runtime. Its CMake project generates `image_shader.h` from `image.metal`; the
kernel compiles this helper through `00-foundation/image_native.cpp` and dispatches
it through `ResultProgramPhase` tensor Needs. `rgba32f/` has no separate
operation-module translation unit or independent registry.

`PixelOE/` is an optional installed-package operation plugin, not a default
built-in registration. Its standalone CMake project finds the Photospider 0.30
`operation_sdk` and `kernel` components. It exports twelve Result ABI 2 image
operations and provides a Result workflow that binds inputs, executes fragment
requests and reads output tensor windows. See [PixelOE's README](PixelOE/README.md)
for build and usage details.

Public behavior and examples are documented in `docs/kernel-architecture`,
including [Basic Operations](../../docs/kernel-architecture/Basic-Operations.md).
