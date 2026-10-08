# Kernel Architecture

These guides describe the kernel's current contracts and runtime behavior. English is authoritative; Chinese reader versions are under [`zh/`](zh/README.zh.md). Start with the [overview](Overview.md) for ownership and execution flow.

## Core model

1. [Overview](Overview.md): module ownership, execution contexts, and result lifetimes.
2. [Terminology](Terminology.md): names used across the architecture guides.
3. [Compiler and Execution](Compiler-and-Execution.md): source validation, planning, scheduling, and run progression.
4. [Data Model](Data-Model.md): Values, Regions, layouts, and retained storage.
5. [Graph Lifecycle](Graph-Lifecycle.md): graph revisions and compiled-stage lifetimes.
6. [Compute Boundaries](Compute-Boundaries.md): CPU/GPU placement and fallback.
7. [Cache Model](Cache-Model.md): frozen inputs and bounded local result reuse.
8. [Plugin ABI](Plugin-ABI.md): trusted in-process operation and provider interfaces.
9. [Image Operations](Image-Operations.md): image descriptors, ports, and current image contracts.
10. [Region Semantics](Region-Semantics.md): logical coverage, input demand, and boundary behavior.

## Workflows and focused topics

- [Result quickstart](../../examples/result_quickstart/README.md): a minimal installed-package operation and workflow.
- [Foundations workflows](../../examples/foundations_workflow/README.md): runnable installed-package examples and their current scenarios.
- [GPU integration tests](../../tests/integration/gpu/): registered GPU behavior coverage; hardware-dependent cases report skip when unavailable.
- [Integer statistics](Integer-Statistics.md): paged histogram and streaming grade operations.
- [External-axis FFT](External-FFT.md): bounded generations and FFT result contracts.
- [Paged connected components](Paged-Components.md): connected-component labels and bounded filtering.

Operation-specific guides describe their own inputs, outputs, and execution rules. The [built-in operations index](../built-in_ops/README.md) routes to those contracts.
