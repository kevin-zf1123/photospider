# ADR 0006: Kernel Documentation Separates Facts, Decisions, Targets, and Implementation Status

## Status

Accepted and applied to the maintained kernel documentation entry points. This
decision does not certify every historical document as compliant and does not
claim that any architecture target has been implemented.

## Context

Kernel documentation serves readers with different questions:

- What does the checked-out software do now?
- Which architecture choices constrain changes, even when their migration is
  incomplete?
- What stable end state should a sequence of changes converge on?
- Which delivery slice is planned, in progress, verified, or complete?

Those questions have different time semantics. Combining them in one living
document makes a target look implemented, turns a decision into a progress
report, or leaves current behavior described by an obsolete migration phase.

The dependency-neutral kernel exposes the ambiguity directly. Public Host and
operation contracts already use Photospider values, while current private
Graph, ROI, dirty propagation, planning, cache, and runtime code still uses
OpenCV geometry or YAML values. The maintained current-state documents record
that implementation. [ADR 0002](0002-external-libraries-are-kernel-adapters.md)
accepts the dependency-neutral constraint without claiming completion, and the
[kernel evolution roadmap](../roadmap/Kernel-Evolution.md) describes the stable
post-migration target. Implementation state belongs to the linked GitHub
Project and Issues. Readers must not have to infer those distinctions from
wording alone.

## Decision

Kernel documentation uses four active information layers:

| Layer | Authority | Time meaning | Allowed content | Prohibited content |
| --- | --- | --- | --- | --- |
| Current facts | `docs/kernel-architecture/` | Behavior and ownership in the current source tree | Observable behavior, implemented ownership and mechanisms, current limitations, invariants, failure semantics, and source/test entry points | Unimplemented designs, migration phases, task status, checkboxes, undated TODOs, or future terms presented as runtime objects |
| Architectural decisions | `docs/adr/` | A durable decision and its decision-time context | Status, context, decision, consequences, rejected alternatives, supersession, and relationships to current facts and targets | Percent complete, transient task lists, or a claim that a target is implemented without matching current facts and evidence |
| Evolution targets | `docs/roadmap/` | Stable accepted direction that may span many delivery slices | Target ownership, boundaries, invariants, dependency order, and required outcomes; an explicitly labelled current baseline when needed to explain the delta | Current behavior presented without a current-state reference, issue status, implementation checklists, schedules, or completion claims |
| Implementation status | Linked GitHub Projects and Issues | The live state and verification of a delivery slice | Tracking key, dependencies, acceptance state, actual commands and results, commit or PR evidence, risks, and follow-up work | Redefinition of current behavior, an architectural decision, or a target solely through a status field or checkbox |

Build, test, and validation procedures remain in `docs/development/` and
`docs/CI/`. Completed migrations, superseded proposals, and phase reports may
be retained under `docs/outdated/` or archived change records, but they are not
a fifth active source of truth.

### Status sources and local work plans

The linked GitHub Project item and Issue are the public implementation-status
source for a roadmap slice. An active OpenSpec change may provide a change-local
proposal, design, specification delta, and task checklist. It does not become
an independent completion authority and must not be required to interpret the
maintained public documentation. Local development boards may mirror or split
follow-up work, but they also do not override Project or Issue state.

No current-state document, ADR, or roadmap carries implementation percentages or
checkboxes. A Project or Issue transition does not by itself make a target a
current fact. Current documentation changes only when source and durable
verification support the new behavior.

### Cross-reference rules

The layers reference rather than copy one another:

1. A current-state document links the governing ADR or roadmap when a boundary
   needs rationale or future context. The link labels the other document as a
   decision or target and does not import future objects into current language.
2. An ADR links the current-state evidence that motivated it and the target it
   constrains. Facts in an ADR's context are historical decision-time facts;
   maintained current behavior remains authoritative in
   `docs/kernel-architecture/`.
3. A roadmap target links its governing ADRs and the current baseline from
   which migration starts. Any baseline summary is explicitly labelled and
   defers to the current-state document.
4. A delivery Issue or PR cites an evidence bundle: the relevant current-state
   document, governing ADR, exact roadmap target, live Project/Issue state, and
   actual verification evidence.

Links stay within the same language where a translated target exists. English
documents are authoritative; the matching `zh/*.zh.md` document is updated in
the same change as a faithful, reader-oriented translation.

### Promotion workflow

Moving a target into current behavior is an explicit promotion, not a wording
change:

1. Before implementation, the delivery slice identifies its current baseline,
   governing decision, target outcome, and live status owner.
2. A change-local design or task plan may refine the work without changing the
   other layers' time meaning.
3. When behavior lands, the same change updates code, long-lived tests, the
   relevant current-state English document, and its Chinese mirror.
4. The roadmap changes only if the accepted target changes. A changed decision
   requires a new or superseding ADR; implementation of an unchanged decision
   does not rewrite its history.
5. Project and Issue state changes only after the required implementation and
   verification evidence exists.

### Application to dependency-decoupling delivery slices

The following routing applies this decision to the child slices of
[Project #2](https://github.com/users/kevin-zf1123/projects/2) and
[parent Issue #51](https://github.com/kevin-zf1123/photospider/issues/51).
It fixes evidence and promotion responsibilities, not completion state. Each
linked child Issue and its Project item remain the implementation-status owner.

| Slice | Current-fact starting evidence | Decision and target use | Promotion responsibility |
| --- | --- | --- | --- |
| [#53 / F-2](https://github.com/kevin-zf1123/photospider/issues/53), reorganize current kernel documents | [Documentation README](../kernel-architecture/README.md), [Terminology](../kernel-architecture/Terminology.md), [Data Model](../kernel-architecture/Data-Model.md), [Dirty Region Propagation](../kernel-architecture/Dirty-Region-Propagation.md), and [Graph Lifecycle](../kernel-architecture/Graph-Lifecycle.md) | Apply this ADR; use the [dependency-neutral target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel) only as the explicit future boundary | Reorganize current facts without importing target-only objects or reporting migration progress in current documents. |
| [#54 / F-3](https://github.com/kevin-zf1123/photospider/issues/54), kernel geometry vertical path | [Dirty Region Propagation](../kernel-architecture/Dirty-Region-Propagation.md) and [Data Model](../kernel-architecture/Data-Model.md) | Apply [ADR 0002](0002-external-libraries-are-kernel-adapters.md) and the [dependency-neutral target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel) | Update current ROI/dirty/planning/execution geometry only after the vertical path and durable tests establish it. |
| [#55 / F-4](https://github.com/kevin-zf1123/photospider/issues/55), `ParameterValue` vertical path | [Data Model](../kernel-architecture/Data-Model.md) and [Graph Lifecycle](../kernel-architecture/Graph-Lifecycle.md) | Apply [ADR 0002](0002-external-libraries-are-kernel-adapters.md) and the [dependency-neutral target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel) | Promote format-neutral parameter behavior only after document, Graph, operation, error, and test evidence agree. |
| [#56 / F-5](https://github.com/kevin-zf1123/photospider/issues/56), remove OpenCV geometry from private interfaces | [Terminology](../kernel-architecture/Terminology.md), [Data Model](../kernel-architecture/Data-Model.md), and [Dirty Region Propagation](../kernel-architecture/Dirty-Region-Propagation.md) | Apply [ADR 0002](0002-external-libraries-are-kernel-adapters.md) and the [dependency-neutral target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel) | Remove the current OpenCV limitation only when Graph/ROI/dirty/planning interfaces and regressions use kernel geometry. |
| [#57 / F-6](https://github.com/kevin-zf1123/photospider/issues/57), kernel buffer primitives | [ImageBuffer Memory Contract](../kernel-architecture/ImageBuffer-Memory-Contract.md), [Data Model](../kernel-architecture/Data-Model.md), and [Dirty Region Propagation](../kernel-architecture/Dirty-Region-Propagation.md) | Apply [ADR 0002](0002-external-libraries-are-kernel-adapters.md) and the [dependency-neutral target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel) | Document current stride-aware tiled normalization and metrics only after padded-row product-path tests pass. |
| [#58 / F-7](https://github.com/kevin-zf1123/photospider/issues/58), optional OpenCV operation provider | [Overview](../kernel-architecture/Overview.md) and [Plugin ABI](../kernel-architecture/Plugin-ABI.md) | Apply [ADR 0002](0002-external-libraries-are-kernel-adapters.md), preserve [ADR 0004](0004-opencv-cpu-operations-are-reentrant-provider-work.md), and use the [dependency-neutral target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel) | Update current module/provider ownership only after initialization, exceptions, algorithms, and replacement evidence sit behind the provider boundary. |
| [#59 / F-8](https://github.com/kevin-zf1123/photospider/issues/59), injected image/artifact codecs | [Cache Model](../kernel-architecture/Cache-Model.md) and [Graph Lifecycle](../kernel-architecture/Graph-Lifecycle.md) | Apply [ADR 0002](0002-external-libraries-are-kernel-adapters.md), preserve [ADR 0005](0005-graph-document-ingestion-is-a-classified-transaction.md), and use the [dependency-neutral target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel) | Promote codec injection only after cache lifecycle and error tests no longer depend on direct OpenCV codec calls. |
| [#60 / F-9](https://github.com/kevin-zf1123/photospider/issues/60), `GraphDefinition` and in-memory document adapter | [Data Model](../kernel-architecture/Data-Model.md) and [Graph Lifecycle](../kernel-architecture/Graph-Lifecycle.md) | Apply [ADR 0002](0002-external-libraries-are-kernel-adapters.md), preserve [ADR 0005](0005-graph-document-ingestion-is-a-classified-transaction.md), and use the [dependency-neutral target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel) | Promote the format-neutral document path only after load/reload/save preserves the classified transaction without temporary YAML. |
| [#61 / F-10](https://github.com/kevin-zf1123/photospider/issues/61), injected YAML filesystem adapter | [Overview](../kernel-architecture/Overview.md) and [Graph Lifecycle](../kernel-architecture/Graph-Lifecycle.md) | Apply [ADR 0002](0002-external-libraries-are-kernel-adapters.md), preserve [ADR 0005](0005-graph-document-ingestion-is-a-classified-transaction.md), and use the [dependency-neutral target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel) | Describe YAML as a current adapter only after composition-root injection and Host format-neutral behavior are verified. |
| [#62 / F-11](https://github.com/kevin-zf1123/photospider/issues/62), remove YAML runtime/cache values | [Data Model](../kernel-architecture/Data-Model.md) and [Cache Model](../kernel-architecture/Cache-Model.md) | Apply [ADR 0002](0002-external-libraries-are-kernel-adapters.md) and the [dependency-neutral target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel) | Remove current YAML runtime/cache statements only after Node, Graph, compute, inspection, and cache interfaces plus regressions use format-neutral values. |
| [#63 / F-12](https://github.com/kevin-zf1123/photospider/issues/63), dependency-disabled build profile | [Overview](../kernel-architecture/Overview.md) and [Testing and Validation](../development/Testing-and-Validation.md) | Use [ADR 0002](0002-external-libraries-are-kernel-adapters.md) as the acceptance constraint and the [dependency-neutral target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel) as the required outcome | Record actual build/install-consumer evidence in the Issue; describe the profile as current only after the dependency-disabled product path passes. |

## Consequences

- Current architecture documents can be used as implementation evidence
  without filtering out future plans.
- ADRs may accept target constraints before migration completes while making
  that status explicit.
- Roadmaps remain stable across individual commits and do not become duplicate
  project boards.
- Dependency-decoupling delivery slices can use one explicit evidence bundle;
  they do not need to infer whether OpenCV/YAML removal is current behavior,
  an accepted constraint, a target, or completed work.
- Reorganizing a current-state document does not authorize moving target-only
  concepts into the current glossary. Conversely, completing a vertical slice
  requires updating the relevant current facts rather than only checking an
  Issue.
- Maintained source documentation remains understandable in a clean primary
  repository even when personal or change-local workflow material is absent.

## Rejected Alternatives

### Keep current behavior and future direction in one living architecture document

Rejected because a paragraph cannot simultaneously be stable target context
and authoritative current behavior without pervasive, fragile temporal labels.

### Use ADR status as implementation status

Rejected because accepting a decision and completing its migration are
different events. ADR 0002 and ADR 0003 intentionally remain accepted target
constraints while their current ownership is documented elsewhere.

### Copy Project checklists into the roadmap

Rejected because duplicated live state drifts and turns a durable target into a
phase report. Roadmaps link the status owner instead.

### Treat a completed Issue as sufficient evidence of current behavior

Rejected because status metadata does not verify source, tests, public
contracts, or documentation. Promotion requires those artifacts to agree.

## Decision Evidence

The initial application of this decision was reviewed against:

- [Kernel Terminology](../kernel-architecture/Terminology.md);
- [Kernel Data Model](../kernel-architecture/Data-Model.md);
- [Dirty Region Propagation and Work Selection](../kernel-architecture/Dirty-Region-Propagation.md);
- [Graph Lifecycle and Mutation Semantics](../kernel-architecture/Graph-Lifecycle.md);
- [ADR 0002](0002-external-libraries-are-kernel-adapters.md); and
- [Kernel Evolution Target](../roadmap/Kernel-Evolution.md).
