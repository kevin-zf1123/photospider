# Repository-owned operations

`plugins/ops/` holds the repository's operation implementations, their public helper headers, and optional operation packages built against the installed kernel SDK. Each registered C++ operation keeps its callback and schema near its source file. `src/lib/plugin/builtin_operations.cpp` aggregates the built-in registration functions and contains no operation callbacks. Private algorithms and addressing helpers stay in internal headers.

| Directory | Responsibility |
| --- | --- |
| `00-foundation` | Core execution probes and the image Result runtime, including native dispatch helpers |
| `01-numeric` | Array arithmetic, curves, expressions, 1D and 3D LUTs (including 3D LUT baking) and smoothstep |
| `02-format-color` | Alpha editing, numeric conversion, transfer, RGB basis and model conversion, channel assembly/editing and metadata assignment |
| `03-generation` | Coordinate and constant fields |
| `05-filter` | Gaussian blur built-in; broader spatial-filter contracts are documented separately |
| `07-grade` | Exposure gain and levels |
| `08-transform` | Image and mask downsampling |
| `09-composite` | Opacity, masks, image mix, source-over and brush stamping |
| `10-analysis` | Histograms and out-of-range counts; the external-axis FFT, paged connected-component and integer-statistics operation factories |
| `include/` | Public helper headers installed as `photospider/ops/` |

## Public helper headers

`include/photospider/ops/` is exported as the header-only `Photospider::ops_headers` target. It contains:

- `format/`: workflow-authoring helpers for the alpha, channel, channel assembly, channel editing, metadata, model conversion, RGB basis and transfer operations;
- `numeric/`: workflow-authoring helpers for the numeric operation families;
- `fft_operation.hpp`, `component_operation.hpp` and `statistics_operation.hpp`: factories that return `OperationDefinition`s for registration by the caller, plus `statistics.hpp` for the statistics data helpers.

`photospider/ops.hpp` includes the kernel umbrella header, all `format/` headers, and the FFT, component and statistics headers. Numeric headers are included individually. Inside this repository, build and test targets link `Photospider::ops_headers` together with the kernel target they test.

## Build and registration

Root CMake lists the built-in operation sources in `PHOTOSPIDER_OPERATION_SOURCES`. It compiles them into the product and test kernels and applies strict floating-point compile options to those sources and their math implementations. The same list includes the `10-analysis` factory implementations and the `01-numeric` 3D LUT baking implementation. So the public helper headers come from `Photospider::ops_headers`, while their symbols come from `Photospider::kernel`. The default C++ registry in `src/lib/plugin/builtin_operations.cpp` registers the built-in families listed above. It does not register the FFT, component or statistics factories; callers register the definitions those factories return. Private helper headers are part of the recursive source inventory, so changing them also changes the cache build identity.

Built-in operations still include private kernel headers from `src/lib/core/` and `src/lib/data/`. A separate built-in operation library needs these dependencies replaced by public SDK interfaces first.

## Optional packages

`rgba32f/` supplies a private Metal shader helper for the shared image Result runtime. Its CMake project generates `image_shader.h` from `image.metal`. The kernel compiles this helper through `00-foundation/image_native.cpp` and dispatches it through `ResultProgramPhase` tensor Needs. `rgba32f/` has no separate operation-module translation unit or registry of its own.

`PixelOE/` is an optional installed-package operation plugin and is not registered by default. Its standalone CMake project finds the Photospider 0.33 `operation_sdk` and `kernel` components. It exports twelve Result ABI 2 image operations and provides a Result workflow that binds inputs, executes fragment requests and reads output tensor windows. See [PixelOE's README](PixelOE/README.md) for build and usage details.

Operation behavior and examples are documented in [`docs/built-in_ops/`](../../docs/built-in_ops/README.md) and in `docs/kernel-architecture/`, including [Basic Operations](../../docs/kernel-architecture/Basic-Operations.md).
