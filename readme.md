# Photospider

Photospider is a C++17, session-agnostic, single-machine graph compiler and
execution kernel for embedding in local applications. The 0.x breaking scope
reset is governed by
[ADR 0015](docs/adr/0015-breaking-product-boundary-scope-reset.md).

## Product surface

The installed `Photospider::kernel` target provides:

- schema-2 `WorkflowDocument` source graphs and immutable per-run `ExecutionBindings`;
- typed semantic IR and optimized IR;
- operation ABI v6 semantic traits with closed typed parameter schemas,
  optimization, and Region-demand-aware local physical planning;
- CPU-required and GPU-optional local execution;
- eight Float32 image/mask operations with bounded runtime scalars;
- explicit regional `Value`, bounded semantic facets, rank-general `Region`,
  strided layout, immutable bytes, Metal shared storage and bounded local caches;
- cooperative cancellation, local resource accounting, fallback, and stale
  completion rejection;
- raw compile/plan/execute diagnostics, named correctness oracle or explicit
  `unchecked` status, and non-security digests.

Independent `GraphContext` and `ExecutionContext` instances may run
concurrently. The kernel defines no daemon Session, Job queue, network service,
durable work, process-worker supervisor, policy DSO, plugin security product,
durable result object, or release-evidence profile.

Operation and data-provider DSOs are trusted in-process extensions configured
at startup. Their exact ABI version/size, alignment, pointer/count, bounded
text/count/rank, closed trait vocabulary, output type/shape/bytes, overflow,
required/exact parameter type, planned input demand, exception, and cleanup
validation remains a correctness boundary.

## Build

The dependency-neutral kernel requires CMake 3.21+, a C++17 compiler, and
Threads. It has no mandatory media, serialization, cryptographic, or GPU SDK
dependency.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CPU exact execution is the default. Explicit `MetalFp32` planning and
`ExecutionContextConfig::gpu_enabled` enable one optional Apple Silicon Metal
queue with completion-safe shared buffers and trait-permitted CPU fallback.
Apple builds use Metal/Foundation privately; set `PHOTOSPIDER_ENABLE_METAL=OFF`
for the CPU configuration. See the [S4 workflow guide](docs/kernel-architecture/S4-Workflow.md)
for installed examples, numeric eligibility, Xcode validation and checkable results.

## Install and consume

```bash
cmake --install build --prefix /desired/photospider-prefix
```

```cmake
find_package(Photospider 0.6 CONFIG REQUIRED COMPONENTS kernel)
target_link_libraries(app PRIVATE Photospider::kernel)
```

Extension authors request only the narrow component they use:

| Use | Component | Target |
| --- | --- | --- |
| Compiler/executor | `kernel` | `Photospider::kernel` |
| Operation ABI headers | `operation_sdk` | `Photospider::operation_sdk` |
| Data-provider ABI header | `data_provider_sdk` | `Photospider::data_provider_sdk` |

There is no policy SDK, worker executable, server component, evidence target,
or legacy preset.

## Local daemon

The separate
[`photospider-daemon`](https://github.com/kevin-zf1123/photospider-daemon)
repository consumes an isolated installation of this package. It owns local
IPC v3 and ephemeral Session/Job orchestration. The daemon has no private
kernel include, copied compiler/planner implementation, internal-IR wire
format, remote endpoint, or plugin path method.

## Documentation

| Need | Start here |
| --- | --- |
| Current ownership and behavior | [Architecture overview](docs/kernel-architecture/Overview.md) |
| Canonical terms | [Kernel terminology](docs/kernel-architecture/Terminology.md) |
| Compiler and local execution | [Compiler and execution](docs/kernel-architecture/Compiler-and-Execution.md) |
| Native Metal workflows | [S4 examples](docs/kernel-architecture/S4-Workflow.md) |
| Runnable image workflow | [Image operations](docs/kernel-architecture/Image-Operations.md) |
| Values and memory | [Data model](docs/kernel-architecture/Data-Model.md) |
| Operation/provider ABI | [Plugin ABI](docs/kernel-architecture/Plugin-ABI.md) |
| Build and validation | [Testing and validation](docs/development/Testing-and-Validation.md) |
| Current delivery sequence | [Current Development Program](docs/development/Current-Development-Program.md) |

English documentation is authoritative. Official documents under `docs/`
have maintained Chinese mirrors in their corresponding `zh/` directories.

## Archive boundary

The pre-reset source is recoverable only from Git history and the annotated
tag `pre-breaking-scope-reset-2026-09-01`. The active tree intentionally has no
compatibility shim, disabled legacy product, or archived-source copy.

## License

Photospider is licensed under the [MIT License](LICENSE).

Copyright (c) 2026 Zhu Feng.
