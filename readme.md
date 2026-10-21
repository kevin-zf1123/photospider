# Photospider

Photospider is a C++17, single-machine graph compiler and execution kernel for embedding in local applications. The kernel owns workflow compilation, planning, local execution, values, and execution resources. See the [architecture overview](docs/kernel-architecture/Overview.md) for current ownership.

## Product surface

The installed `Photospider::kernel` target provides:

- schema-7 `WorkflowDocument` source graphs and immutable per-run `ExecutionBindings`;
- immutable validated IR with optional ordered optimization;
- operation module ABI 8, fixed invocation ABI 8 and OperationTraits 30, with closed typed parameters and immutable physical Task/Tile/Call/Block planning;
- CPU-required and GPU-optional fixed execution through `prepare`, `submit` and `join`, with dependency-driven issue and ordered call retirement;
- operation support is defined by each registered contract; see [image operations](docs/kernel-architecture/Image-Operations.md) and the [built-in operations index](docs/built-in_ops/README.md);
- static `Result` members and unified `Tensor`, explicit operation-bound validation, rank-general `Region`, affine layout, immutable bytes, native Shared/Device storage and explicit COPY tasks;
- cooperative cancellation with accepted-work drain, stale-state checks, bounded admission and independent Run payload/metadata observations;
- raw compile/plan/execute diagnostics, timing, counters, resource observations and non-security digests.

Independent `GraphContext` and `ExecutionContext` instances may run concurrently. The kernel defines no daemon Session, Job queue, network service, durable work, process-worker supervisor, policy DSO, plugin security product, durable result object, or release-evidence profile.

Operation DSOs are trusted in-process extensions configured at startup. Their exact ABI version/size, alignment, pointer/count, bounded text/count/rank, closed trait vocabulary, output type/shape/bytes, overflow, required/exact parameter type, static read/write contracts, fixed completion ownership, exception, and cleanup validation remains a correctness boundary.

## Build

Building the kernel requires CMake 3.21+, Clang C and C++ compilers with C99 and C++17 support, and Threads. Bundled operations additionally require the pinned SLEEF 3.9.0 source; follow the [SLEEF preparation instructions](third_party/SLEEF.md) to download it into `third_party/sleef/`. The `psc` and `psx` tools, enabled by default, require the pinned yyjson 0.13.0 source; follow the [yyjson preparation instructions](third_party/YYJSON.md) to place it in `third_party/yyjson/` before configuring, or set `PHOTOSPIDER_BUILD_TOOLS=OFF` to disable the tools. Both source directories are ignored by Git and absent from repository checkouts; CMake does not download dependencies. Reuse an existing build directory for local development; configure only when its options need changing. The kernel library itself has no mandatory media, serialization, cryptographic, or GPU SDK dependency.

```bash
CC=clang CXX=clang++ cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build --target photospider -j 8
# Build affected test targets, then run a focused CTest expression.
```

CPU exact execution is the default. `ExecutionMode::NativeGpu` selects GPU lowering for operations that advertise GPU support and CPU lowering for CPU-only operations. A lowering may explicitly mix CPU/GPU tasks within a call when its registered capabilities permit them. The resulting Plan fixes every placement and explicit COPY edge. `maximum_gpu_submissions` bounds native submission credits; zero rejects preparation of GPU tasks. Numerical behavior belongs to each operation/profile. Preparation and execution do not retry a failed GPU placement on CPU. Apple builds use Metal/Foundation privately; set `PHOTOSPIDER_ENABLE_METAL=OFF` for the CPU configuration. See [Compiler and Execution](docs/kernel-architecture/Compiler-and-Execution.md) for the current execution contracts.

`PHOTOSPIDER_BUILD_BENCHMARK_TOOLS` defaults to `OFF`. Its optional
`Photospider::benchmark_tools` target depends only on public kernel APIs. The
kernel umbrella does not export benchmark interfaces. Retained tests and temporary
acceptance follow [Testing and Validation](docs/development/Testing-and-Validation.md).

## Compiler and executor tools

`PHOTOSPIDER_BUILD_TOOLS=ON` builds `psc` and `psx` by default. `psc input.json -o output.psxp` validates and plans directly without optimization. Add `-O optimize` explicitly for the optional example pass; `--emit ir` and `--save-ir N=PATH` select and preserve IR versions. `psx output.psxp --bindings inputs.json -o output.results` executes saved physical tables and exports actual Tensor coverage. Built-in operations register by default when ops are enabled; otherwise use `--load-ops`.

See [CLI usage](docs/development/Compiler-Tools.md), [file formats](docs/development/Compiler-Tool-Formats.md), and [中文说明](docs/development/zh/Compiler-Tools.zh.md). Artifacts use v5; earlier formats require recompilation/re-export.

## Install and consume

The following is package usage documentation, not a development validation step.
Installation-consumer tests remain prohibited until kernel development completes.

```bash
cmake --install build --prefix /desired/photospider-prefix
```

```cmake
find_package(Photospider 0.39 CONFIG REQUIRED COMPONENTS kernel)
target_link_libraries(app PRIVATE Photospider::kernel)
```

Consumers request only the components they use:

| Use | Component | Target |
| --- | --- | --- |
| Compiler/executor | `kernel` | `Photospider::kernel` |
| Built-in operations and authoring implementations | `ops` | `Photospider::ops` |
| Built-in operation helper headers | `ops_headers` | `Photospider::ops_headers` |
| Operation ABI headers | `operation_sdk` | `Photospider::operation_sdk` |
| Operations-domain SDK | `ops_sdk` | `Photospider::ops_sdk` |
| Optional benchmark runner, oracle and reports | `benchmark_tools` | `Photospider::benchmark_tools` |

`Photospider::ops` supplies bundled numeric, curve/LUT, channel, alpha and color
operations and their authoring implementations. It depends on the shared domain
SDK target `Photospider::ops_sdk`, which uses only kernel public interfaces.
`Photospider::ops_headers` supplies the public authoring include tree; it does
not supply compiled implementations on its own.

```cmake
find_package(Photospider 0.39 CONFIG REQUIRED COMPONENTS kernel ops)
target_link_libraries(app PRIVATE Photospider::ops)
```

The build options `PHOTOSPIDER_BUILD_OPS` and `PHOTOSPIDER_BUILD_OPS_SDK` control
these libraries. Building ops requires the SDK. An SDK-only build may enable the
SDK while disabling ops; consumers request the `ops_sdk` component and link
`Photospider::ops_sdk` when that library was built.

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
| Operation ABI | [Plugin ABI](docs/kernel-architecture/Plugin-ABI.md) |
| Build and validation | [Testing and validation](docs/development/Testing-and-Validation.md) |
| Architecture decisions | [ADR index](docs/adr/README.md) |

English documentation is authoritative. Official documents under `docs/adr/`, `docs/kernel-architecture/`, `docs/development/`, and other `docs/` areas that contain a `zh/` directory have maintained Chinese mirrors there. `docs/built-in_ops/` is maintained in Chinese without a separate mirror.

## License

Photospider is licensed under the [MIT License](LICENSE).

Copyright (c) 2026 Zhu Feng.

Named-output API and host tests: [Multi-output operations](docs/kernel-architecture/Multi-Output-Operations.md).
