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
| Delivery status | Public GitHub Issues | Concrete tasks, dependencies, actual verification, risks, and completion state |

ADR 0015 is the highest active product-boundary decision. A lower-level fact,
Issue, Project field, OpenSpec note, or historical tag cannot override it.
There is no active roadmap layer after the breaking reset; retained compiler
and heterogeneous-execution work is described by maintained architecture and
narrowed live tracking.

[`docs/development/Current-Development-Program.md`](../development/Current-Development-Program.md)
is a checked-in public snapshot of the current baseline, milestone, critical
path, and active leaf Issues. It cannot change architecture and GitHub's live
Issue state prevails when the snapshot is stale. GitHub Projects are
maintainer operational views that mirror Issues and cannot override them.

OpenSpec files in the private personal overlay are maintainer working notes.
They have no public architecture or delivery authority, do not gate public
completion, and become effective only when accepted content is promoted into
the applicable public ADR, current-fact or development document, and GitHub
Issue.

### Promotion workflow

Moving planned behavior into current facts requires one coherent change:

1. implement the behavior and long-lived tests;
2. update the relevant English current-fact document and Chinese mirror;
3. update affected ADRs only when the decision changed;
4. update live Issue/Project state and the checked-in delivery snapshot with
   actual test results.

A status checkbox alone never proves current behavior. Conversely, code is not
complete when installed contracts and maintained documentation still describe
the superseded boundary.

### Archive rules

Git history, annotated tags, and archived OpenSpec changes may preserve
historical text. Active indexes must not link an archive as current authority.
Removed domains are labelled removed, out of scope, or archive-only; they are
not described as later, future, deferred, optional, or default-disabled.

### Language parity

English public documents are authoritative. Every maintained official public
document has a faithful reader-oriented Chinese mirror updated in the same
change. A mirror does not introduce additional requirements. Private working
notes follow personal-overlay policy and are outside the public parity gate.

## Consequences

- Current architecture can be read without reconstructing a migration history.
- Breaking retirement is explicit rather than hidden behind stale links.
- Delivery records cite real code and test results without becoming product
  authority.
- A clean primary clone remains understandable without personal workflow data.
- Maintainers may use OpenSpec privately without making public review depend on
  unavailable material.
