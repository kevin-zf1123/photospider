# Graph Lifecycle

## 1. Scope and ownership

`GraphContext` owns a copied `WorkflowDocument`, a monotonic revision, and shared currentness state used by snapshots and compiled stages. Construction publishes the initial source at revision 1. The caller owns the context object and must keep it alive while calls use it. Graph construction stores source; compilation performs semantic validation.

## 2. Snapshot data

```cpp
class GraphContext {
 public:
  GraphSnapshot snapshot() const;
  std::uint64_t replace(WorkflowDocument document);
};
```

`snapshot()` captures a coherent source and revision. `replace()` prepares the replacement before publishing it under the context lock. On success it advances the revision, so older snapshots and plans become stale. A failed replacement leaves the existing source and revision unchanged.

## 3. State transitions and execution

```text
constructed at revision r -> snapshot(r) -> analyze / optimize / plan
            |                                      |
            +-- replace succeeds -> revision r+1  +-> execution checks currentness
            |                                                   |
            +-- destroy -> snapshots non-current                +-> Stale, discard result

IRProgram -> bind_ir -------------+
.psir / .psxp -> import / load ---+-> standalone stage, revision 1
                                  |      current while the registry lives
                                  +-> execution with the same frozen registry
```

Compiler stages retain currentness identity but do not retain mutable access to the source document. A stage that `Compiler::analyze` or `Compiler::compile` builds from a snapshot records that snapshot's revision, and its `current()` follows the snapshot. `optimize`, `plan` and `select_outputs` copy the parent's currentness, so every stage derived from the snapshot becomes stale together. An execution that observes replacement or context destruction returns `Stale`; final result publication checks currentness again after callbacks complete.

## 4. Standalone stages

`Compiler::bind_ir` and the `CompilerArtifact` importers produce stages that have no `GraphContext`. Their `revision()` is 1, and their `current()` reports whether the frozen `OperationRegistry` they were linked against is still alive. Replacing or destroying any `GraphContext` does not affect them. A saved `.psir` or `.psxp` file carries no snapshot, revision or source liveness; saving a snapshot-bound stage and loading it again yields a standalone stage at revision 1.

Every stage, standalone or snapshot-bound, keeps a weak reference to its registry. `optimize` and `plan` reject a stage linked to another registry with `Stale`, and `ExecutionContext::prepare` rejects a Plan whose registry differs from the context's registry with `Stale`. `submit` requires a PreparedPlan originating from that context and rechecks source currentness. A host that loads an artifact therefore registers or loads the required operations, freezes the registry, builds both the `Compiler` and the `ExecutionContext` over that same registry, and keeps it alive while the stages are in use.

## 5. Failure and lifetime

Replacement allocation or revision overflow fails without changing published state. Destruction marks outstanding snapshots non-current. Destruction does not own or stop an `ExecutionContext`; callers preserve ordinary C++ lifetime safety for every object used by an in-progress call.

## 6. Limitations and non-goals

The kernel provides no document filesystem adapter, implicit working directory, durable graph identifier, or persistence service for `GraphContext`. Compiler artifact files persist IR and plan stages only; they read and write the explicit path given by the caller. `GraphContext` revision is local compiler state and does not identify a daemon Session.
