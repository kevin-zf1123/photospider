# Compiler and Package Version Contract

## Current contract

Package 0.39.0 uses SameMinorVersion compatibility and SOVERSION `0.39`. C++ consumers and external C operation modules must rebuild against matching headers. Operation module ABI 8 and fixed invocation ABI 8 require exact record sizes. The loader requires `ps_operation_module_get_api_v8`; a missing entry returns `TypeMismatch`, while a malformed module version or size returns `InvalidArgument`.

| Contract | Version |
| --- | --- |
| Package | 0.39.0 |
| WorkflowDocument | 7 |
| OperationTraits | 30 |
| Operation module C ABI | 8, exact-size |
| Fixed kernel invocation C ABI | 8, exact-size |
| Native kernel preparation service ABI | 2 |
| `.psir` / `.psxp` text format | 5 |
| Result schema encoding | `photospider.result-schema.v6` |
| Validated IR | `ordered-retirement-ir-v5` |
| Example optimizer | `optimize@1` (provenance only) |
| Physical plan | `physical-task-program-v5` |
| Saved operation contract | `fixed-operation-contract-v3` |

These version axes are independent. The module ABI describes static preparation, physical lowering and kernel preparation. The invocation ABI describes immutable inputs, exclusive outputs and scratch, cancellation and completion. Native service 2 prepares Metal or Vulkan kernels over Shared and Device storage; explicit GPU COPY tasks also support Host endpoints through admitted Shared staging. Its availability depends on the build and selected device. See the [plugin ABI](../kernel-architecture/Plugin-ABI.md) for ownership, sizes and supported domains.

## Compiler stages and artifacts

Frontend/`bind_ir`, optional named optimizers and artifact import publish the same immutable `ValidatedIR`. The planner accepts DependencyPaths or ControlFlow. For dependency paths it deduplicates Call IDs in a private lowering copy. CFG loops retain their dynamic activation semantics. The default compile pipeline has zero optimizer invocations. Ordered optimizer invocations, including duplicate names, create numbered revisions; content identity excludes producer history, so a no-op can preserve its digest.

Artifact v5 uses `stage ir` and explicit program form for `.psir`; `.psxp` saves lowered CFG and complete physical task tables, with its actual input IR identity as parent. Both raw and transformed IR can be imported and planned directly. Plan import links and validates saved tables without lowering again. Registry/resource matching and currentness checks remain. Operation manifest digests describe contracts, not code authentication. See [tools](Compiler-Tools.md), [formats](Compiler-Tool-Formats.md) and [compiler architecture](../kernel-architecture/Compiler-and-Execution.md).

`CompiledWorkflow` contains ordered IR revisions and an optional Plan. `ExecutionPlan::input_ir_digest()` identifies its parent IR. Consumers must rebuild for the current public API and re-export artifacts written in versions 1–4. Artifact import rejects those versions.

## Workflow inputs and operation execution

Workflow declarations require static Result schemas. Bindings supply owning `ResultRef` objects by exact name. The Plan fixes schemas, tile geometry, placements, task dependencies, kernel symbols and block control flow. Its tile extent defaults to 128. Lowering converts requested outputs and explicit input reads into physical tasks; shared computation is represented by tasks and private values within the call.

`ExecutionContext::prepare` links the Plan into a `PreparedPlan`, resolves kernels and prepares native capacities. `submit` creates a Run with its own binding snapshot, cancellation and dynamic activation state. CPU kernels return synchronously; accepted GPU kernels finish through their completion callback. Ready tasks may execute across calls. CallGroups retire in IR order, and final RET exposes the selected Results after preceding calls have retired. Failure and cancellation drain accepted work before its borrowed storage can be released.

## Result and image interface

A Result has 1..16 named static Tensor members. Dtype, positive shape, rank, batch/atomic axes and topology are fixed before execution, including statically derived extents. Runtime counts and masks are ordinary payload inside fixed capacity.

Tensor backing supports fragments, legal signed/zero strides, external owners and sparse physical coverage. Owning windows expose actual runs; explicit collection and packing consume capacity and work. The Plan's input layout contract determines the representation accepted by a Run. CPU COPY tasks can explicitly connect Host and Shared tiles. Image numerical semantics belong to operation implementations and helpers.

Static C callbacks provide schemas, selected resources and a physical operation program. Runtime kernels borrow fixed views and scratch for the invocation lifetime. The host validates structural contracts during compilation, preparation and binding preflight; kernels own numerical correctness and must stay within their declared accesses.

## Identity and runtime matching

Canonical IR identities include program form, identifiers, prepared operation contracts, static schemas, parameters, selected resources and control flow. Plan identity additionally includes layouts, placements, task reads/writes, dependencies, scratch requirements, CallGroups, block reservations, return signatures and output selection. Renumbering calls or values changes these identities. Raw pointers, padding and allocation addresses do not define identity.

Compiler artifacts are editable versioned text. They contain neither native pointers nor library paths, device handles, Run state or cache contents. Runtime module/device ownership remains in the linked `PreparedPlan` and its kernels.
