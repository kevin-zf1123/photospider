# ADR 0006: Documentation Keeps Current Facts Separate from Decisions

- Status: Accepted

## 1. Core Summary (TL;DR)
Architecture documentation describes behavior visible in the current source tree. ADRs record durable boundaries and their reasons. Keeping these roles separate lets engineers use current contracts without reconstructing delivery history.

## 2. Mental Model & Intuition

```text
typed public headers + implementation --> current behavior documentation
accepted architecture constraints ------> ADRs
```

Both documentation types describe the same product from different angles: current facts explain what callers can do today, while an ADR explains why a lasting boundary exists.

## 3. Formal Contracts & APIs

```cpp
// Public declarations and behavior are authoritative for current API facts.
#include <photospider/photospider.hpp>
```

`docs/kernel-architecture/` documents implemented ownership, interfaces, invariants, and limitations. `docs/adr/` records accepted decisions, rationale, consequences, and boundaries. When the implementation changes, current-fact documentation follows the checked-out headers and implementation. An ADR changes when its architectural decision changes, not to mirror every implementation detail. English documents are authoritative; maintained Chinese mirrors convey the same contract.

## 4. Non-Goals & Explicit Boundaries
- ADRs do not serve as task tracking, release status, or a roadmap.
- Architecture documentation does not claim behavior absent from the implementation.
- Private working notes and external issue trackers are not public API authorities.

## 5. Consequences
Readers can identify whether a statement describes current behavior or a design constraint. Maintainers update the document whose role changed and avoid stale delivery details becoming implied product behavior. A discrepancy between headers and current-fact documentation requires checking the implementation before either source is changed.
