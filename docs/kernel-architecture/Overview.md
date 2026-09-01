# Architecture Overview

Photospider is a C++17 embeddable compiler/execution kernel for local graphs.
Callers own independent `GraphContext` objects and choose independent or shared
`ExecutionContext` objects.

## Pipeline

```text
WorkflowDocument -> GraphContext/GraphSnapshot
  -> Compiler::analyze -> SemanticGraphIR
  -> Compiler::optimize -> OptimizedGraphIR
  -> Compiler::plan -> ExecutionPlan
  -> ExecutionContext::execute -> ExecutionResult
```

Every compiler stage returns a complete immutable value or one failure. Source,
semantic, optimized, plan, runtime Value, and daemon identities are separate.

## Module ownership

| Module | Current ownership |
| --- | --- |
| graph | `GraphContext`, `GraphSnapshot`, copied source revision/currentness |
| compiler | validation, typed IR, conservative no-op optimization, local plan, typed digests/key |
| execution | bounded CPU pool, optional GPU callback lane, private Run, cancellation, byte ledger, raw diagnostics |
| data | dense immutable `Value`, rank-general `Region`, `StridedLayout` |
| plugin | exact operation/data-definition C ABIs and startup-frozen registries |
| benchmark | raw compile/plan/execute observations and correctness-oracle result |

CPU execution is required. Optional GPU support is a configured local callback
lane selected only for operations that declare it. Cross-backend inputs are
explicit immutable copies; no native GPU SDK is required by the kernel package.

Cancellation is cooperative. Plan currentness and cancellation are checked at
completion and before final result return. Resource leases and intermediate
Values use ordinary exact C++ ownership.

The daemon depends on the installed public package. The kernel never depends
on daemon source, serializes no internal IR, and owns no daemon namespace or
Job/result lifecycle.
