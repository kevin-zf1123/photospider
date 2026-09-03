# Photospider Kernel Context

Photospider is a session-agnostic, single-machine, embeddable graph compiler
and execution kernel. [ADR 0015](docs/adr/0015-breaking-product-boundary-scope-reset.md)
is the highest active product-boundary authority.

## Ubiquitous language

Read [Kernel Terminology](docs/kernel-architecture/Terminology.md) before naming
public or private concepts. The core pipeline is:

```text
WorkflowDocument
  -> SemanticGraphIR
  -> OptimizedGraphIR
  -> ExecutionPlan
  -> ExecutionContext
  -> ExecutionResult
```

- `GraphContext` owns one source graph and monotonic revision. It is not a
  daemon Session or a global registry entry.
- `ExecutionRun` owns one request's immutable inputs, cancellation, staged
  output, completion arbitration, and diagnostics.
- `ExecutionContext` owns bounded local CPU/GPU execution resources.
- `Value`, bounded facets, `Region`, strided layout, immutable bytes, and
  Run-local cross-backend copies are explicit runtime contracts.
- Operation ABI v2 semantic traits publish required/exact parameter schemas and
  Whole/Elementwise/Halo Region rules that drive typed validation,
  optimization, physical plan demands, and callback input legality.

## Non-negotiable distinctions

- A source document is not semantic IR.
- Semantic IR is not optimized IR.
- Optimized IR is not a physical plan.
- A plan digest is a non-security compiler identity, not a runtime allocation
  or daemon identity.
- A graph context is not a daemon Session.
- An ExecutionRun is not a daemon Job.
- A ready task is not a task graph.
- CPU execution is required; GPU execution is an optional local backend.
- Cancellation is cooperative and stale completion cannot publish.
- Derived caches are disposable and rebuildable.
- ABI validation is correctness validation, not a sandbox or trust product.

## Product boundary

The kernel owns compilation, optimization, local physical planning, local
execution, runtime Values, local resource accounting, cancellation, raw
benchmarks, and trusted in-process operation/provider loading.

The separate `photospider-daemon` product owns same-user local IPC v3,
`SessionId`, ephemeral `JobId`, queue/status/cancel/result/release, and daemon
lifecycle. It consumes only an isolated installation of this package and never
serializes internal IR.

Network service, authentication, tenant isolation, durable work, recovery,
process workers, plugin sandboxing, policy DSOs, cryptographic plugin
admission, durable result objects, and release evidence are removed or out of
scope. They are not future or default-disabled kernel features.

## Active documentation

- Current facts: `docs/kernel-architecture/`
- Highest kernel product-boundary authority:
  `docs/adr/0015-breaking-product-boundary-scope-reset.md`
- Companion daemon product-boundary authority, where daemon responsibilities
  apply:
  `photospider-daemon/docs/adr/0001-breaking-product-boundary-scope-reset.md`
- Active OpenSpec authority:
  - `openspec/specs/product-boundary-authority/spec.md`
  - `openspec/specs/kernel-compiler-execution/spec.md`
  - `openspec/specs/daemon-local-orchestration/spec.md`
  - `openspec/specs/raw-benchmark-diagnostics/spec.md`
- Delivery status: narrowed GitHub Issues and Projects
- Historical evidence only:
  `openspec/changes/archive/2026-09-03-breaking-product-boundary-reset/`

English documents are authoritative. Official Chinese mirrors are maintained
in the matching `zh/` paths.
