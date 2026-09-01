# ADR 0006: Documentation Separates Facts, Decisions, and Delivery Status

- Status: Accepted, narrowed by ADR 0015
- Date: 2026-09-01 boundary revision

## Context

Readers need to distinguish implemented behavior, governing architectural
decisions, and current delivery status. Mixing those time meanings makes an
unimplemented target look current or lets an obsolete roadmap continue to
authorize removed product domains.

## Decision

Active documentation uses three layers:

| Layer | Authority | Content |
| --- | --- | --- |
| Current facts | `docs/kernel-architecture/` | Behavior, ownership, invariants, limitations, and source/test entry points in the checked-out tree |
| Decisions | `docs/adr/` | Accepted boundaries, rationale, consequences, supersession, and explicit non-goals |
| Delivery status | GitHub Issues/Projects plus an active OpenSpec change | Concrete tasks, dependencies, actual verification, risks, and completion state |

ADR 0015 is the highest active product-boundary decision. A lower-level fact,
Issue, Project field, archived OpenSpec, or historical tag cannot override it.
There is no active roadmap layer after the breaking reset; retained compiler
and heterogeneous-execution work is described by maintained architecture,
OpenSpec, and narrowed live tracking.

### Promotion workflow

Moving planned behavior into current facts requires one coherent change:

1. implement the behavior and long-lived tests;
2. update the relevant English current-fact document and Chinese mirror;
3. update affected ADRs only when the decision changed;
4. update active OpenSpec tasks and live Issue/Project state with actual test
   results.

A status checkbox alone never proves current behavior. Conversely, code is not
complete when installed contracts and maintained documentation still describe
the superseded boundary.

### Archive rules

Git history, annotated tags, and archived OpenSpec changes may preserve
historical text. Active indexes must not link an archive as current authority.
Removed domains are labelled removed, out of scope, or archive-only; they are
not described as later, future, deferred, optional, or default-disabled.

### Language parity

English documents are authoritative. Every maintained official document and
OpenSpec artifact has a faithful reader-oriented Chinese mirror updated in the
same change. A mirror does not introduce additional requirements.

## Consequences

- Current architecture can be read without reconstructing a migration history.
- Breaking retirement is explicit rather than hidden behind stale links.
- Delivery records cite real code and test results without becoming product
  authority.
- A clean primary clone remains understandable without personal workflow data.
