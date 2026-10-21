# Kernel Architecture

English is authoritative; Chinese reader versions are under [`zh/`](zh/README.zh.md). The [Compiler and Executor design](Compiler-and-Execution.md), [parallel model](Parallel-Execution-Model.md), [tile storage](Cache-Model.md), [dependencies](Dependency-Data.md) and [terminology](Terminology.md) specify the breaking replacement. Their proposal status is independent of implementation progress.

The current C++ path compiles physical Task/Tile/Call/Block tables and executes prepared CPU kernels. [Plugin ABI](Plugin-ABI.md) documents operation module ABI 8, fixed invocation and ownership. [Compiler tools](../development/Compiler-Tools.md) documents the v5 artifact CLI. [Fragment atlas](Fragment-Atlas.md) describes the independent Tensor packing utility. The fixed executor prepares and runs real Metal and Vulkan kernels, including Shared/Device storage and explicit COPY. [Native measurements](Parallel-Execution-Model.md#native-measurements) record the workload, capacity, timing and drain evidence. Existing ADRs remain decision records; the replacement scope is enumerated in the Plan design.

## Core model

1. [Overview](Overview.md): module ownership, execution contexts, and result lifetimes.
2. [Terminology](Terminology.md): names used across the architecture guides.
3. [Compiler and Execution](Compiler-and-Execution.md): static Task/Tile/Call/Block tables, device prepare, ordered retirement and breaking artifact contracts.
4. [Data Model](Data-Model.md): Tensors, Regions, layouts, and retained storage.
5. [Graph Lifecycle](Graph-Lifecycle.md): graph revisions and compiled-stage lifetimes.
6. [Compute Boundaries](Compute-Boundaries.md): fixed CPU/GPU placement, explicit transfers and completion ownership.
7. [Cache Model](Cache-Model.md): unique tile allocations, lifetime holds and block admission without payload caches.
8. [Plugin ABI](Plugin-ABI.md): trusted in-process operation interfaces.
9. [Image Operations](Image-Operations.md): image descriptors, ports, and current image contracts.
10. [Region Semantics](Region-Semantics.md): logical coverage, input demand, and boundary behavior.

## Workflows and focused topics

- [Executor semantic prototype](prototypes/ooo_executor.py): run with Python 3; `--test` checks scheduling, retirement, failure and buffer ownership. GPU completion is simulated.

- [Compiler artifacts](Compiler-and-Execution.md#compiler-artifacts): `.psir`/`.psxp` v5 boundaries and checked import.
- [Version contract](../development/Compiler-Version-Contract.md): independent package, ABI, schema, artifact and identity versions.
- [Basic operations](Basic-Operations.md): bundled numeric, color and image inputs, outputs and execution rules.
- [Managed resources](Managed-Resources.md): budgets, ownership, retention and release.
- [Atom errors and quality](Atom-Errors-and-Quality.md): failure scope and numerical quality.
- [Global results](Global-Results.md): static Tensor members, captured coverage, owning publication and result lifetimes.

- [Fixed native integration](../../tests/integration/kernel/test_fixed_native.cpp) and [Vulkan checks](../../tests/unit/kernel/test_vulkan_gpu.cpp): real native behavior coverage; hardware-dependent cases report skip when unavailable.

Operation-specific guides describe their own inputs, outputs, and execution rules. The [built-in operations index](../built-in_ops/README.md) routes to those contracts.
