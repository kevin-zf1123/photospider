# Graph Lifecycle

`GraphContext` owns a copied `WorkflowDocument`, a nonzero monotonic revision,
and shared currentness state. It is independently allocated and has no kernel
name or global registry.

Construction publishes the caller's source at revision one. Semantic
validation occurs later in `Compiler::analyze`; construction does not claim
that the document compiles.

`snapshot()` copies a coherent document/revision pair. `replace()` first
allocates a new immutable document, then advances the revision and swaps source
under one lock. Allocation or revision-overflow failure leaves state unchanged.
A successful replacement monotonically makes every older snapshot, IR, and
plan stale.

Context destruction marks outstanding snapshots non-current. An executing Run
that observes that change returns `Stale` and rejects late output. Destruction
does not own, stop, or join a shared `ExecutionContext`; callers must retain
normal C++ lifetime safety while compile/execute calls use their objects.

The kernel has no document filesystem adapter, implicit directory, durable
graph identity, Session lifetime, or persistence service.
