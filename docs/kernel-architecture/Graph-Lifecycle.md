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
```

Compiler stages retain currentness identity but do not retain mutable access to the source document. An execution that observes replacement or context destruction returns `Stale`; final result publication checks currentness again after callbacks complete.

## 4. Failure and lifetime

Replacement allocation or revision overflow fails without changing published state. Destruction marks outstanding snapshots non-current. Destruction does not own or stop an `ExecutionContext`; callers preserve ordinary C++ lifetime safety for every object used by an in-progress call.

## 5. Limitations and non-goals

The kernel provides no document filesystem adapter, implicit working directory, durable graph identifier, or persistence service. `GraphContext` revision is local compiler state and does not identify a daemon Session.
