# Kernel Architecture

These guides describe the kernel's current contracts and runtime behavior. English is authoritative; Chinese reader versions are under [`zh/`](zh/README.zh.md). Start with the [overview](Overview.md) for ownership and execution flow.

## Core model

1. [Overview](Overview.md): module ownership, execution contexts, and result lifetimes.
2. [Terminology](Terminology.md): names used across the architecture guides.
3. [Compiler and Execution](Compiler-and-Execution.md): DAG/SSA stages, fixed tile planning, sequential VM execution, and editable artifacts.
4. [Data Model](Data-Model.md): Tensors, Regions, layouts, and retained storage.
5. [Graph Lifecycle](Graph-Lifecycle.md): graph revisions and compiled-stage lifetimes.
6. [Compute Boundaries](Compute-Boundaries.md): CPU/GPU placement and fallback.
7. [Cache Model](Cache-Model.md): immutable input snapshots and bounded local result reuse.
8. [Plugin ABI](Plugin-ABI.md): trusted in-process operation interfaces.
9. [Image Operations](Image-Operations.md): image descriptors, ports, and current image contracts.
10. [Region Semantics](Region-Semantics.md): logical coverage, input demand, and boundary behavior.

## Workflows and focused topics

- [Compiler artifacts](Compiler-and-Execution.md#compiler-artifacts): `.psir`/`.psxp` v2 save/load APIs, manifest linking, validation, and a checked-error example.
- [Version contract](../development/Compiler-Version-Contract.md): independent package, ABI, schema, artifact and identity versions.
- [Basic operations](Basic-Operations.md): bundled numeric, color and image inputs, outputs and execution rules.
- [Managed resources](Managed-Resources.md): budgets, ownership, retention and release.
- [Atom errors and quality](Atom-Errors-and-Quality.md): failure scope and numerical quality.
- [Global results](Global-Results.md): static Tensor members, publication, fixed grants and result lifetimes.

- [GPU integration tests](../../tests/integration/kernel/gpu/): registered GPU behavior coverage; hardware-dependent cases report skip when unavailable.

Operation-specific guides describe their own inputs, outputs, and execution rules. The [built-in operations index](../built-in_ops/README.md) routes to those contracts.
