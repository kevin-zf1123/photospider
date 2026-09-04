# Kernel Architecture

These documents describe current kernel behavior after the breaking product
boundary reset. English is authoritative; faithful Chinese mirrors are under
[`zh/`](zh/README.zh.md).

[ADR 0015](../adr/0015-breaking-product-boundary-scope-reset.md) is the highest
active product-boundary authority.

Public live delivery state is maintained in GitHub Issues. GitHub Projects are
maintainer operational views of those Issues. The checked-in
[Current Development Program](../development/Current-Development-Program.md)
summarizes the baseline and execution order without changing this architecture.

## Reading order

1. [Overview](Overview.md)
2. [Terminology](Terminology.md)
3. [Compiler and Execution](Compiler-and-Execution.md)
4. [Data Model](Data-Model.md)
5. [Graph Lifecycle](Graph-Lifecycle.md)
6. [Compute Flow](Compute-Flow.md)
7. [Compute Boundaries](Compute-Boundaries.md)
8. [Cache Model](Cache-Model.md)
9. [Region Semantics](Region-Semantics.md)
10. [Plugin ABI](Plugin-ABI.md)

The kernel is session-agnostic and owns no daemon Job, network service,
durable work, process supervisor, policy DSO, plugin security product, durable
result object, or release evidence. Pre-reset documents are available only
through Git history and `pre-breaking-scope-reset-2026-09-01`.
