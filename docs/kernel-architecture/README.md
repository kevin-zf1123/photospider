# Kernel Architecture Documentation

This directory describes the kernel that exists in the current source tree.
Its audience is developers who need to understand observable behavior,
ownership, implementation mechanisms, invariants, failure semantics, and the
source locations that enforce those facts.

## Information Boundaries

[ADR 0006](../adr/0006-kernel-documentation-separates-facts-decisions-targets-and-status.md)
governs the documentation information architecture. Its four active layers
have distinct authorities and time meanings:

| Layer | Authoritative location | Time meaning |
| --- | --- | --- |
| Current facts | `docs/kernel-architecture/` | Behavior and ownership in the current source tree. |
| Architectural decisions | `docs/adr/` | Durable decisions and their decision-time context. |
| Evolution targets | `docs/roadmap/` | Stable accepted direction, not a claim about current behavior. |
| Implementation status | Linked GitHub Projects and Issues | Live state, dependencies, and verification of one delivery slice. |

Build, test, and validation guidance remains in `docs/development/` and
`docs/CI/`. An active OpenSpec change may hold a change-local plan and
checklist, but it is not an independent public completion authority. Completed
migrations and obsolete proposals may be retained in `docs/outdated/` or
archived change records; they are historical, not active sources of truth.

Kernel architecture documents contain observable behavior, implemented
ownership and mechanisms, current limitations, invariants, failure semantics,
and source/test entry points. They must not contain task checkboxes,
implementation phase reports, migration status tables, undated TODO lists, or
future runtime objects. A future concept enters this directory only after code
and durable verification make it current software behavior.

## Cross-Reference and Update Rules

- Current documents may link ADRs for rationale and roadmaps for explicitly
  labelled future context; those links do not make target objects current.
- ADR context is a historical decision-time snapshot. Maintained behavior
  remains authoritative here, even when an accepted ADR migration is
  incomplete.
- Roadmaps may summarize an explicitly labelled current baseline, but must
  defer to the corresponding current document and link the governing ADR.
- Each implementation Issue or PR cites the relevant current document,
  governing ADR, exact roadmap target, live Project/Issue state, and actual
  verification evidence.
- When a target behavior lands, the same change updates code, long-lived tests,
  the affected English current-state document, and its Chinese mirror. A
  changed target updates the roadmap; a changed decision requires a new or
  superseding ADR. A status transition alone changes none of those layers.

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
