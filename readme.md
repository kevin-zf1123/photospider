# Photospider

Photospider is a C++17, single-machine graph compiler and execution kernel for embedding in local applications. The kernel owns workflow compilation, planning, local execution, values, and execution resources. See the [architecture overview](docs/kernel-architecture/Overview.md) for current ownership.

## Product surface

The installed `Photospider::kernel` target provides:

- schema-6 `WorkflowDocument` source graphs and immutable per-run `ExecutionBindings`;
- typed semantic IR and optimized IR;
- Result operation C ABI 4 and OperationTraits 26 with closed typed parameter schemas, optimization, and Region-demand-aware local physical planning;
- CPU-required and GPU-optional local execution;
- operation support is defined by each registered contract; see [image operations](docs/kernel-architecture/Image-Operations.md) and the [built-in operations index](docs/built-in_ops/README.md);
- structural `Result`/regional `Value`, explicit operation-bound validation, rank-general `Region`, strided layout, immutable bytes, Metal shared storage and bounded local caches;
- cooperative cancellation, local resource accounting, fallback, and stale completion rejection;
- raw compile/plan/execute diagnostics, named correctness oracle or explicit `unchecked` status, and non-security digests.

Independent `GraphContext` and `ExecutionContext` instances may run concurrently. The kernel defines no daemon Session, Job queue, network service, durable work, process-worker supervisor, policy DSO, plugin security product, durable result object, or release-evidence profile.

Operation and data-provider DSOs are trusted in-process extensions configured at startup. Their exact ABI version/size, alignment, pointer/count, bounded text/count/rank, closed trait vocabulary, output type/shape/bytes, overflow, required/exact parameter type, planned input demand, exception, and cleanup validation remains a correctness boundary.

## Build

Building the kernel requires CMake 3.21+, Clang C and C++ compilers with C99 and C++17 support, and Threads. Bundled operations additionally require the pinned SLEEF 3.9.0 source. For an ops build, follow the [SLEEF preparation instructions](third_party/SLEEF.md) to download it into `third_party/sleef/`. This ignored directory is not included in the repository; CMake does not download dependencies. Configure in a new, empty `build/` directory so the build and install commands below use the same tree. There is no mandatory media, serialization, cryptographic, or GPU SDK dependency.

```bash
CC=clang CXX=clang++ cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CPU exact execution is the default. `ExecutionMode::NativeGpu` grants native placement to operations whose traits declare a GPU implementation; `ExecutionContextConfig::gpu_enabled` enables the context's configured Metal or Vulkan backend. The mode selects placement, while each operation/profile defines its numerical behavior, and traits govern CPU fallback. Apple builds use Metal/Foundation privately; set `PHOTOSPIDER_ENABLE_METAL=OFF` for the CPU configuration. See [Compiler and Execution](docs/kernel-architecture/Compiler-and-Execution.md) for the current execution contracts.

## Install and consume

```bash
cmake --install build --prefix /desired/photospider-prefix
```

```cmake
find_package(Photospider 0.34 CONFIG REQUIRED COMPONENTS kernel)
target_link_libraries(app PRIVATE Photospider::kernel)
```

Consumers request only the components they use:

| Use | Component | Target |
| --- | --- | --- |
| Compiler/executor | `kernel` | `Photospider::kernel` |
| Built-in operations and authoring implementations | `ops` | `Photospider::ops` |
| Built-in operation helper headers | `ops_headers` | `Photospider::ops_headers` |
| Operation ABI headers | `operation_sdk` | `Photospider::operation_sdk` |

`Photospider::ops` supplies bundled numeric, curve/LUT, channel, alpha and color
operations and their authoring implementations. It depends on the shared domain
SDK target `Photospider::ops_sdk`, which uses only kernel public interfaces.
`Photospider::ops_headers` supplies the public authoring include tree; it does
not supply compiled implementations on its own.

```cmake
find_package(Photospider 0.34 CONFIG REQUIRED COMPONENTS kernel ops)
target_link_libraries(app PRIVATE Photospider::ops)
```

The build options `PHOTOSPIDER_BUILD_OPS` and `PHOTOSPIDER_BUILD_OPS_SDK` control
these libraries. Building ops requires the SDK. An SDK-only build may enable the
SDK while disabling ops; consumers find the `kernel` component and link the
exported `Photospider::ops_sdk` target when that library was built. `ops_sdk` is
a target, not a separately recognized package component.

`#include <photospider/ops.hpp>` includes the kernel umbrella and bundled helper
headers. Numeric helpers are included individually, for example
`photospider/ops/numeric/lut3d_baking.hpp`. Domain configuration, ICC/OCIO resource
wrappers, numeric reports, LUT bake readers and Layer arithmetic are under
`photospider/ops/`. Results carry no attached domain description: consumers pass
color, sampling and LUT configuration explicitly. Kernel-only applications use
`photospider/photospider.hpp` and register their own operations.

## Local daemon

The separate [`photospider-daemon`](https://github.com/kevin-zf1123/photospider-daemon) repository owns local IPC and ephemeral Session/Job orchestration, while the kernel owns compilation, execution, values, and execution resources. The daemon consumes the installed public kernel package; compatibility with a particular daemon revision is not established by this repository.

## Documentation

| Need | Start here |
| --- | --- |
| Current ownership and behavior | [Architecture overview](docs/kernel-architecture/Overview.md) |
| Canonical terms | [Kernel terminology](docs/kernel-architecture/Terminology.md) |
| Compiler and local execution | [Compiler and execution](docs/kernel-architecture/Compiler-and-Execution.md) |
| Image contracts and execution boundaries | [Image operations](docs/kernel-architecture/Image-Operations.md) |
| Values and memory | [Data model](docs/kernel-architecture/Data-Model.md) |
| Operation/provider ABI | [Plugin ABI](docs/kernel-architecture/Plugin-ABI.md) |
| Build and validation | [Testing and validation](docs/development/Testing-and-Validation.md) |
| Architecture decisions | [ADR index](docs/adr/README.md) |

English documentation is authoritative. Official documents under `docs/adr/`, `docs/kernel-architecture/`, `docs/development/`, and other `docs/` areas that contain a `zh/` directory have maintained Chinese mirrors there. `docs/built-in_ops/` is maintained in Chinese without a separate mirror.

## License

Photospider is licensed under the [MIT License](LICENSE).

Copyright (c) 2026 Zhu Feng.

Named-output API and host tests: [Multi-output operations](docs/kernel-architecture/Multi-Output-Operations.md).
