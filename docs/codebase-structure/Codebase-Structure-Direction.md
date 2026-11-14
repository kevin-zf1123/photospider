# Codebase Structure Direction

ADR 0015 defines the repository boundary. The kernel owns an embeddable compiler/executor and trusted in-process operation extensions. The `psc` and `psx` applications compile and execute local files; daemon orchestration belongs to `photospider-daemon`.

## Public layout

The public include trees install under the `photospider/` prefix. Optional libraries control installation of their own headers.

```text
include/photospider/
  compiler/       WorkflowDocument, IR, Plan, compiler stages and artifact codec
  execution/      execution context, bindings, cancellation and diagnostics
  data/           Result, Tensor, Region, layouts and immutable resources
  plugin/         operation ABI, registry and trusted host services
  core/           status, resources, cancellation and symbol export
  ops/            optional operations-domain SDK
  benchmark/      optional benchmark client API
  photospider.hpp embedded-kernel convenience include

plugins/ops/include/photospider/
  ops/            bundled operation factories and workflow-authoring helpers
  ops.hpp         kernel umbrella and bundled helper headers
```

The kernel installation excludes `ops/` and `benchmark/`; `cmake/OpsSdk.cmake`, `cmake/BundledOperations.cmake` and `cmake/BenchmarkTools.cmake` own those optional exports. `photospider.hpp` includes kernel APIs only. Domain configuration and operation-specific names belong to the ops SDK or bundled authoring headers.

Public headers use public kernel types and retain native implementation objects privately. Consumers do not need `src/lib` headers or a sibling checkout.

## Compiler models and stages

| Public header | Responsibility |
| --- | --- |
| `compiler/workflow_document.hpp` | DAG declarations, `GraphContext` and coherent snapshots. |
| `compiler/ir.hpp` | SSA/CFG data, dependency paths, `ValidatedIR`, content identity and provenance. |
| `compiler/plan.hpp` | Planning options, fixed instructions, grants, `ExecutionPlan` and physical identities. |
| `compiler/compilation.hpp` | Retained IR revisions, optional Plan and stage timings. |
| `compiler/frontend.hpp` | Frozen frontend catalog and typed DAG lowering callbacks. |
| `compiler/optimizer.hpp` | Frozen optimizer catalog, parameter schemas and independent transformations. |
| `compiler/planner.hpp` | Frozen planner catalog and callback-local `PlanBuilder`. |
| `compiler/compiler.hpp` | Stage orchestration over frozen registries. |
| `compiler/artifact.hpp` | IR/Plan text import, export, inspection and file helpers. |

Execution headers depend directly on the Plan model. Callers that instantiate `Compiler` include `compiler/compiler.hpp` or the kernel umbrella explicitly. The default `Compiler` constructor assembles frontend, optimizer and planner catalogs; the `CompilerComponents` constructor accepts an explicit component selection. Plan import uses the frozen operation catalog and `ProgramServices` without constructing a compiler.

## Source ownership and applications

| Source tree | Responsibility |
| --- | --- |
| `src/lib/core/`, `src/lib/data/` | Status, cancellation, resource accounting, Tensor/Result storage, views and Region primitives. Data includes only core/data headers. |
| `sdk/model/` | Workflow document/context and physical Task/Tile/Call/Block models and validation. |
| `sdk/operation/` | Operation registration, C ABI adapters, static preparation, checked program construction and mapping dispatch. |
| `sdk/program/` | IR binding, Plan linking/validation/selection, canonical identities and artifact codecs. |
| `sdk/execution/` | Provider-facing execution API, Plan/data services, binding preflight and host thread retirement. |
| `plugins/compiler/default/` | Compiler stage orchestration and frontend/optimizer/planner dispatch. |
| `plugins/frontends/dag/`, `plugins/optimizers/deduplicate/`, `plugins/planners/static_tiles/` | Concrete DAG frontend, optional deduplication and static tile planner. |
| `plugins/executors/physical/` | Physical scheduling, block memory, cache, CPU workers and optional native GPU execution. |
| `plugins/mappers/geometry/` | Compile-time geometry solvers and their registration/module entries. |
| `host/` | Catalog assembly, default component factories, provider loading, planner module bridge and host-owned Metal callbacks. |
| `apps/common/`, `apps/psc/`, `apps/psx/`, `apps/benchmark/` | Shared CLI I/O, compiler driver, executor driver and optional benchmark client. |
| `plugins/ops/common/domain/`, `plugins/ops/<NN-family>/` | Operations-domain SDK and bundled operation implementations. |
| `tests/fixtures/`, `tests/unit/kernel/`, `tests/integration/kernel/` | Synthetic ABI producers and maintained kernel behavior tests. |
| `tests/package/`, `tests/tools/` | Package metadata contracts and application behavior tests. |

Plan import in `sdk/program/` links saved declarations and verifies the physical tables without invoking a frontend, optimizer, mapper or planner. The execution SDK consumes that bound Plan and an explicitly selected provider. The physical provider owns scheduling and GPU implementation dependencies. `src/lib/core/` and `src/lib/data/` have no reverse dependency on the compiler, operation catalog or executor.

The common CLI library owns neither a compiler session nor an execution context. `psc` links compiler and compile-time mapper components; `psx` links execution components and can import `.psxp` without a compiler or mapper catalog. Their entry points dispatch into separate typed driver libraries. CLI JSON uses the private, pinned yyjson source described in [YYJSON.md](../../third_party/YYJSON.md); its declarations do not enter public headers.

Full stage/lifetime contracts are in [Compiler and Execution](../kernel-architecture/Compiler-and-Execution.md). File and application contracts are in [Compiler Tools](../development/Compiler-Tools.md) and [Compiler Tool Formats](../development/Compiler-Tool-Formats.md).

## Targets and dependency direction

The component definitions in `cmake/KernelComponents.cmake` establish these boundaries. Component names below use the `Photospider::` namespace; executable and private target names are written explicitly.

| Target | Build condition | Role |
| --- | --- | --- |
| `data`, `model` | always | Storage/resource primitives and physical/workflow models. |
| `plugin_host`, `operation_runtime`, `program` | always | Provider lifetime/loading, operation contracts and bound Plan/artifact services. |
| `operation_sdk` | always | Header-only C operation/provider ABI; pure C producers need no kernel linkage. |
| `compiler`, `compiler_default` | `PHOTOSPIDER_BUILD_COMPILER=ON` | Compiler orchestration; the default assembly adds DAG, deduplicate and static-tiles providers. |
| `frontend_dag`, `optimizer_deduplicate`, `planner_static_tiles`, `mapper_geometry` | compiler enabled | Independently linkable compile-time implementations. |
| `execution`, `executor_physical`, `execution_default` | `PHOTOSPIDER_BUILD_EXECUTOR=ON` | Provider-facing execution SDK, physical implementation and their default assembly. |
| `metal_host` | executor and Metal enabled on Apple | Host-owned Metal completion and page-release callbacks. |
| `kernel` (`photospider`) | compiler and executor enabled | Interface convenience target over `compiler_default` and `execution_default`. |
| `ops_sdk` | `PHOTOSPIDER_BUILD_OPS_SDK=ON` | Domain contracts, immutable resources and numerical helpers. |
| `ops_runtime`, `ops_headers` | `PHOTOSPIDER_BUILD_OPS=ON` | Runtime registration/implementations and public bundled-operation headers. |
| `ops` | ops and compiler enabled | Runtime ops plus workflow authoring helpers and the default geometry mapper catalog. |
| `benchmark_tools` | `PHOTOSPIDER_BUILD_BENCHMARK_TOOLS=ON` | Optional raw benchmark client. |
| `psc`, `psx` | tools enabled plus the corresponding compiler/executor option | Separate compiler and executor executables. |
| `result_files` | tools and executor enabled | Installed static Result file-export library. |
| `photospider_cli_common`, `photospider_cli_json` | tools enabled | Installed/exported support archives for CLI I/O and JSON. |
| `photospider_psc`, `photospider_psx`, test fixtures | build only | Private drivers and focused behavior tests. |

Compiler, executor and tools default to ON; bundled operations and benchmark tools default to OFF. Bundled operations require the ops SDK. CLI environment code registers `ops_runtime` when enabled. The compile-time `ops` convenience library adds `compiler_default` and `mapper_geometry`; execution-only use of `ops_runtime` does not introduce those dependencies.

A compiler-only build disables `PHOTOSPIDER_BUILD_EXECUTOR`; an execution-only build disables `PHOTOSPIDER_BUILD_COMPILER`. The latter does not create `test_cli_io`, which exercises both compiler and executor APIs. Dynamic provider tests link `compiler_default` and `execution` explicitly and build the provider module as a separate dependency, so the host does not preload the physical executor.

Shared `ops` retains the package VERSION/SOVERSION and hidden implementation visibility. Its static form retains position-independent code. Kernel components have no dependency on CLI drivers, bundled operations or daemon source. Installation-consumer testing remains excluded during kernel development; retained package tests check package metadata. Native modules are trusted in-process extensions.

## Naming and documentation

Types use `PascalCase`; functions, fields, files and internal targets use `snake_case`. Compiler extensions have explicit catalog names independent of their implementation file paths. A rename updates declarations, definitions, includes, tests, CMake and affected English/Chinese documents together.

Public API documentation specifies applicable parameters, errors, ownership, lifetime, thread safety and cache or scheduling constraints. Internal comments explain constraints such as immutable stage publication, resource retention and reclamation order. Product documentation describes current behavior; historical recovery notes remain separate from the maintained contracts.
