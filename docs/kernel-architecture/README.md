# Kernel Architecture Documentation

This directory describes the kernel that exists in the current source tree.
Its audience is developers who need to understand observable behavior,
ownership, implementation mechanisms, invariants, failure semantics, and the
source locations that enforce those facts.

## Information Boundaries

Each kind of information has one home:

| Information | Authoritative location |
| --- | --- |
| Current kernel behavior and implementation | `docs/kernel-architecture/` |
| Accepted architectural decisions | `docs/adr/` |
| Stable future architecture goals | `docs/roadmap/` |
| Build, test, and validation guidance | `docs/development/` and `docs/CI/` |
| Implementation tasks and current progress | GitHub Projects, Issues, and active OpenSpec changes |
| Completed migrations and obsolete proposals | `docs/outdated/` and archived OpenSpec changes |

Kernel architecture documents must not contain task checkboxes, implementation
phase reports, migration status tables, or undated TODO lists. A future concept
is documented here only after it becomes current software behavior. Before
that point it belongs in a roadmap or ADR.

## Reading Order

1. [Overview](Overview.md) explains product seams, module ownership, and the
   top-level call graph.
2. [Terminology](Terminology.md) defines the current domain language and terms
   that must not be conflated.
3. [Data Model](Data-Model.md) and [Graph Lifecycle](Graph-Lifecycle.md) explain
   graph state, topology, persistence behavior, and mutation semantics.
4. [Compute Boundaries](Compute-Boundaries.md) and
   [Compute Flow](Compute-Flow.md) explain planning, pruning, dispatch, HP/RT
   intent, and commit behavior.
5. [Cache Model](Cache-Model.md) and
   [Dirty Region Propagation](Dirty-Region-Propagation.md) define cache
   authority, ROI mathematics, dirty state, and tile mapping.
6. [Scheduler Architecture](Scheduler-Architecture.md) defines the current
   ready-task scheduler interface and physical worker ownership.
7. [ImageBuffer Memory Contract](ImageBuffer-Memory-Contract.md) and
   [Plugin ABI](Plugin-ABI.md) define memory, device, operation, scheduler, and
   DSO contracts.

The accepted post-merge direction is described separately in
[Kernel Evolution](../roadmap/Kernel-Evolution.md). It is not evidence of
current implementation.

## Document Shape

A maintained domain document should answer, as applicable:

1. What terms and state belong to the domain?
2. What behavior can callers observe?
3. Who owns state and how long does it live?
4. Which current modules and call paths implement the behavior?
5. Which invariants and prohibited dependencies define the boundary?
6. Why is the mechanism designed this way?
7. How are errors, cancellation, and partial work exposed?
8. Which source and long-lived tests provide the implementation entry points?

English documents are authoritative. Files under `zh/` are faithful,
reader-oriented Chinese translations and are updated in the same change.
