# Repository-owned operations

`plugins/ops/` holds the repository's operation implementations, their public helper headers, and optional operation packages built against the installed kernel SDK. Each registered C++ operation keeps its callback and schema near its source file. `plugins/ops/registry.cpp` aggregates the built-in registration functions and contains no operation callbacks. Private algorithms and addressing helpers stay in internal headers.

| Directory | Responsibility |
| --- | --- |
| `01-numeric` | Array arithmetic, curves, expressions, 1D and 3D LUTs (including 3D LUT baking) and smoothstep |
| `02-format-color` | Alpha editing, numeric conversion, transfer, RGB basis and model conversion, channel assembly/editing and metadata assignment |
| `include/` | Public helper headers installed as `photospider/ops/` |

## Public helper headers

`include/photospider/ops/` is exported as the header-only `Photospider::ops_headers` target. It contains:

- `format/`: workflow-authoring helpers for the alpha, channel, channel assembly, channel editing, metadata, model conversion, RGB basis and transfer operations;
- `numeric/`: workflow-authoring helpers for the numeric operation families;

`photospider/ops.hpp` includes the kernel umbrella header, all `format/` headers, and the FFT, component and statistics headers. Numeric headers are included individually. Inside this repository, build and test targets link `Photospider::ops_headers` together with the kernel target they test.

## Build and registration


Built-in operations still include private kernel headers from `src/lib/core/` and `src/lib/data/`. A separate built-in operation library needs these dependencies replaced by public SDK interfaces first.

## Optional packages

`PixelOE/` is an optional installed-package operation plugin and is not registered by default. Its standalone CMake project finds the Photospider 0.33 `operation_sdk` and `kernel` components. It exports twelve Result ABI 3 image operations and provides a Result workflow that binds inputs, executes fragment requests and reads output tensor windows. See [PixelOE's README](PixelOE/README.md) for build and usage details.

Operation behavior and examples are documented in [`docs/built-in_ops/`](../../docs/built-in_ops/README.md) and in `docs/kernel-architecture/`, including [Basic Operations](../../docs/kernel-architecture/Basic-Operations.md).
