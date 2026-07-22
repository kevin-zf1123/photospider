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
migrations and obsolete proposals are indexed by the
[outdated-document archive](../outdated/README.md) or retained in archived
change records; they are historical evidence, not active sources of truth.

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

## Documentation Map

Use the maintained documents by question rather than by file age. A document
may support more than one lens, but each fact has one primary home:

| Lens | Question answered | Primary documents |
| --- | --- | --- |
| Terminology | What do current names and states mean, and which concepts must remain distinct? | [Terminology](Terminology.md) and [Data Model](Data-Model.md) |
| Behavior | What can a caller observe during graph lifecycle, compute, cache, and dirty-region work? | [Graph Lifecycle](Graph-Lifecycle.md), [Compute Flow](Compute-Flow.md), [Cache Model](Cache-Model.md), and [Dirty Region Propagation](Dirty-Region-Propagation.md) |
| Implementation | Which current modules own the behavior, and what is the call or dispatch path? | [Overview](Overview.md), [Compute Boundaries](Compute-Boundaries.md), and [Policy and Execution Architecture](Policy-and-Execution-Architecture.md) |
| Boundaries | Which values, ownership rules, invariants, limitations, and failure surfaces may consumers rely on? | [ImageBuffer Memory Contract](ImageBuffer-Memory-Contract.md), [Plugin ABI](Plugin-ABI.md), and [Compute Boundaries](Compute-Boundaries.md) |
| Rationale | Why is the current mechanism separated this way, and which durable decisions constrain it? | Rationale sections in the current documents and the governing [ADRs](../adr/) |

A first-time reader should start with [Terminology](Terminology.md), then read
[Overview](Overview.md), and follow the behavior or boundary document for the
subsystem being changed. Source and long-lived test entry points at the end of
each domain document provide the evidence trail for current claims.

The accepted post-merge direction is described separately in
[Kernel Evolution](../roadmap/Kernel-Evolution.md). It is a target, not evidence
of current implementation. Historical phase reports and migration plans remain
under the [outdated-document archive](../outdated/README.md) and must be checked
against the maintained current documents before use.

## Current-Document Shape

A maintained domain document uses the following order where the lens applies:

1. terms and current state;
2. observable behavior;
3. current implementation and ownership;
4. boundaries, invariants, limitations, and failure semantics;
5. rationale for the current mechanism; and
6. source and long-lived test entry points.

Domain-specific headings may refine those lenses, but they must not turn a
future target into a current object or duplicate a migration checklist. When a
document needs future context, it states the current limitation first and then
links the exact roadmap target or governing ADR.

English documents are authoritative. Files under `zh/` are faithful,
reader-oriented Chinese translations and are updated in the same change.
