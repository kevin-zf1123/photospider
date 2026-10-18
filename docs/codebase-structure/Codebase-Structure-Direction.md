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

Execution headers depend directly on the Plan model. Callers that instantiate `Compiler` include `compiler/compiler.hpp` or the kernel umbrella explicitly. Compiler construction and artifact import initialize only the catalogs required by the requested stage.

| Private source | Responsibility |
| --- | --- |
| `src/lib/compiler/compiler.cpp` | Registry ownership and analyze/optional-optimize/plan orchestration. |
| `frontend.cpp`, `frontend_registry.cpp`, `frontend/DAG/dag.cpp` | Frontend dispatch, catalog and DAG dependency-path lowering. |
| `ir_validation.cpp`, `dependency_paths.hpp` | Common IR validation, operation binding and private path lowering. |
| `optimizer.cpp`, `optimizer_registry.cpp`, `optimizers/deduplicate.cpp` | Transform invocation, output validation, catalog and optional deduplication. |
| `planner.cpp`, `planner_registry.cpp`, `planners/static_tiles.cpp` | Backend dispatch, catalog and static instruction generation. |
| `plan_linking.cpp`, `plan_validation.cpp`, `plan_selection.cpp` | Declaration/placement linking, saved instruction verification and coverage narrowing. |
| `static_mapping.cpp`, `identity.cpp` | Static dependency helpers and canonical content/cache identities. |
| `artifact.cpp`, `artifact_text.cpp` | Artifact linking and bounded text grammar. |

The source names in the last seven rows are relative to `src/lib/compiler/`. Plan import links saved declarations and verifies saved code without invoking an optimizer, mapper or planner. Full stage and lifetime contracts are in [Compiler and Execution](../kernel-architecture/Compiler-and-Execution.md).

## Private layout and applications

```text
src/lib/
  core/          checked arithmetic, status factories and resource primitives
  data/          Tensor/Result storage, views, regions and budgets
  compiler/      stage implementations and artifact codecs
  graph/         workflow document/context implementation
  execution/     Plan interpreter, scheduling, publication and cache
  plugin/        registries, DSO loading and C ABI host adapters

src/tools/
  common/        CLI parsing, diagnostics, signals, JSON and file/manifests I/O
  psc/           compiler driver, action graph, IR session and DAG JSON input
  psx/           executor driver, runtime bindings and Result export
  benchmark/     optional raw benchmark client implementation

plugins/ops/common/domain/ operations-domain SDK implementations
plugins/ops/<NN-family>/   bundled operation implementations and helpers

tests/fixtures/            synthetic operations and ABI modules
tests/unit/kernel/       kernel behavior tests
tests/integration/kernel/ kernel execution and ABI behavior tests
tests/package/           package metadata contracts
tests/tools/             application and optional benchmark behavior tests
```

`src/lib/core/` contains primitives independent of other kernel modules. Data implementations include only core/data headers. Compiler stages use operation metadata and static preparation; execution consumes the Plan and invokes operation programs. The common CLI library owns neither a compiler session nor an execution context. `psc` and `psx` have separate typed invocations and driver libraries; the entry points only dispatch into those drivers.

CLI JSON uses the private, pinned yyjson source described in [YYJSON.md](../../third_party/YYJSON.md). Its declarations do not enter installed kernel headers. File formats and application contracts are documented in [Compiler Tools](../development/Compiler-Tools.md) and [Compiler Tool Formats](../development/Compiler-Tool-Formats.md).

## Targets and dependency direction

| Target | Installation condition | Role |
| --- | --- | --- |
| `photospider` / `Photospider::kernel` | always | Embedded compiler/executor and operation ABI host. |
| `Photospider::operation_sdk` | always | Header-only trusted operation DSO ABI. |
| `Photospider::ops_sdk` | `PHOTOSPIDER_BUILD_OPS_SDK=ON` | Domain contracts, resources and numerical helpers over public kernel APIs. |
| `Photospider::ops`, `Photospider::ops_headers` | `PHOTOSPIDER_BUILD_OPS=ON` | Separate bundled implementation library and authoring include root. |
| `Photospider::benchmark_tools` | `PHOTOSPIDER_BUILD_BENCHMARK_TOOLS=ON` | Raw benchmark client over public kernel APIs. |
| `psc`, `psx` | `PHOTOSPIDER_BUILD_TOOLS=ON` | Compiler and executor executables. |
| `photospider_cli_json`, `photospider_cli_common`, `photospider_psc`, `photospider_psx` | build only | Private CLI support and driver libraries. |
| Test executables and fixture modules | build only | Maintained program behavior and package contracts. |

Tools default to ON; bundled operations and benchmark tools default to OFF. Bundled operations require the ops SDK. Enabling ops also links their registration into the CLI environment; a kernel-only CLI accepts explicitly loaded operation libraries.

```text
psc driver -> compiler/artifact APIs -> operation metadata and preparation
psx driver -> artifact/execution APIs -> operation runtime programs
both drivers -> common CLI I/O -> private yyjson

bundled ops -> ops SDK -> public kernel APIs
benchmark client -> public kernel APIs
photospider-daemon -> installed Photospider::kernel
```

The kernel has no dependency on tool drivers, bundled operations or daemon source. Installation-consumer testing is excluded during kernel development; retained package tests check package metadata. Native operation loading is a trusted in-process boundary.

## Naming and documentation

Types use `PascalCase`; functions, fields, files and internal targets use `snake_case`. Compiler extensions have explicit catalog names independent of their implementation file paths. A rename updates declarations, definitions, includes, tests, CMake and affected English/Chinese documents together.

Public API documentation specifies applicable parameters, errors, ownership, lifetime, thread safety and cache or scheduling constraints. Internal comments explain constraints such as immutable stage publication, resource retention and reclamation order. Product documentation describes current behavior; historical recovery notes remain separate from the maintained contracts.
