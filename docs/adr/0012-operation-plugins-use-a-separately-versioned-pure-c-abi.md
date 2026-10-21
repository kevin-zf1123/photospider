# ADR 0012: Operation Plugins Use Versioned C Contracts

- Status: Accepted
- Implementation note (2026-10-20): the pure C boundary remains the accepted decision. Package 0.39.0 implements operation module ABI 8 and fixed invocation ABI 8. [ADR 0022](0022-freeze-operation-c-abi-v3.md) remains a historical Proposed continuation design; its staged records and prefix-negotiation proposal are not the current ABI.

## 1. Core summary

Operation plugins cross the kernel boundary through versioned C interfaces. The host validates and copies declarations before publishing registry entries. C modules and C++ operations use the same static lowering, physical Plan validation and fixed invocation contracts. Native plugins remain in the host process and trust domain.

The current contracts are specified in [Plugin ABI](../kernel-architecture/Plugin-ABI.md) and [Compiler, Plan and Executor](../kernel-architecture/Compiler-and-Execution.md). Their implementation status does not change the formal status of the broader physical-execution proposal.

## 2. Ownership and stages

Startup loading validates the module table and copies traits, schemas and parameter contracts into a private registry candidate. Publication is atomic; the registry is frozen before compiler and executor use.

Static preparation specializes schemas and captures immutable state and declared resources. Lowering constructs fixed tasks, reads, writes, placement and scratch; it never receives sample payload. Explicit execution preparation links kernels and native pipelines without changing topology. CPU invocation returns only after access ends; accepted GPU invocation owns its borrowed views through exactly one terminal completion.

Module code remains mapped while any definition, static preparation or prepared kernel needs it. Release callbacks run before the module's destroy callback and native unload. Prepared kernels retain static state and resources, and a Run retains preparation through cancellation drain. Query/service records are borrowed for their callback; retained data must be copied into the provider's own state.

## 3. Formal contracts

```c
#include "photospider/plugin/operation_module_api.h"

PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void);
```

The standalone C11 header defines exact-size module ABI 8 and includes fixed invocation ABI 8. OperationTraits version is 30. Native shader preparation uses the separately versioned native kernel service ABI 2. There is no current data-provider C table.

A module declares operation contracts and static prepare/resource-validation/lowering/kernel-preparation callbacks. The host validates versions and record sizes, alignment, pointer/count pairs, bounded strings and arrays, closed enum/flag combinations, checked arithmetic, required callbacks and cleanup ownership. The module header is checked before attaching its destroy hook and reading nested records. All declarations validate in a private candidate; a malformed declaration, duplicate key or freeze race publishes no partial registry update.

Lowering records schema-only and payload reads separately, unique output producers, private SSA intermediates, explicit COPY tasks and at most one terminal effect task per effectful call. Kernel preparation validates the actual compiled invocation views, parameters, scratch and selected placement before returning an unchecked direct entry point. Execution performs no continuation polling, runtime dependency expansion, publication grants, numerical certification or backend retry.

A rejected asynchronous submission keeps ownership with the coordinator and emits no callback. Acceptance retains input/output/scratch, parameter and module owners until matching completion establishes quiescence and visibility, including cancellation. Completion may occur inline. Callbacks and release functions obey their nonthrowing contract; malformed keys cannot settle another submission.

## 4. Boundaries

ABI validation is not sandboxing, signature verification, package admission or crash isolation. Native code can still crash or hang the host. Daemon IPC does not expose operation-plugin loading as a capability.

The current tables have no ABI 1–7 getter, prefix-size adapter, staged continuation shim or automatic translation. External consumers must rebuild against the matching headers and package.

## 5. Consequences

Each public boundary carries its own compatibility version. Loading rejects a mismatched module before registry publication. Compiler artifacts preserve static contracts and physical tables but contain no native pointers. Artifact import and explicit device preparation remain separate. Installed-consumer testing is suspended during kernel development; focused C11 module, loader, preparation and fixed-execution checks exercise this boundary.
