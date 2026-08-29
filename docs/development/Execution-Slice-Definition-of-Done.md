# Execution Slice Definition of Done

## Status and authority

This document is the common completion and readiness contract for future
implementation slices in Projects #7 through #14. It is a target-planning
contract, not evidence that a capability exists. Current behavior remains in
`docs/kernel-architecture/`; accepted decisions remain in `docs/adr/` and
English OpenSpec; live state remains in GitHub Issues and Projects.

An Issue keeps only its slice-specific facts and links this document for common
governance. Program and aggregate Issues may use an equivalent program-level
form, but must maintain a real descendant checklist and completion closure.

## Required Issue structure

```markdown
## Current baseline
Current authoritative objects, observable behavior, limitations, source entry
points, and maintained tests. Future targets must not be stated as current.

## Required human decisions
Decisions an AFK agent must not make: architecture authority, ABI/schema,
migration, semantic legality, security boundary, numeric target, or product
scope. Link the governing ADR/OpenSpec when accepted.

## Authority map
Owner:
Non-owners:
Input authority:
Output/commit authority:
Persistence authority:

## Public and schema impact
Public C++ API:
Plugin ABI:
IPC/API:
Document schema:
Artifact schema:
Migration policy:

## Minimal vertical
Name one fixture or corpus, one real input-to-authoritative-execution path, one
observable result, and the exact boundary deliberately excluded.

## Failure and fallback
Unknown semantic:
Unsupported backend:
Resource exhaustion:
Cancellation:
Stale result:
Persistence failure:
Rollback:

## Exact dependencies
### Start
Contracts required before design or implementation.

### Integration
Only real upstream prerequisite verticals whose output is required to join this
slice to the real product path. Do not list downstream adopters,
demonstrations, or cross-slice validation here.

### Consumers / integration validation
Downstream adopters, joint demonstrations, cross-slice validation, package
consumers, and evidence that this slice is consumable. This section creates no
dependency edges.

### Completion
Evidence or conformance required before this Issue may close. Completion is a
closure gate, not an execution edge. A Project parent belongs here only when
full Project closure is genuinely required.

## Verification
Unit:
Integration:
Package consumer:
Differential oracle:
Benchmark:
Fault injection:
Platform matrix:

## Completion evidence
Exact commands:
Expected artifacts:
Review gates:
Documentation updates:
Known limitations:

## Project traceability
Native parent:
Native descendants:
Projects:
Phase / Target / Risk / Work Type / Verification:
```

## Readiness promotion checklist

`Work Type=AFK|HITL` describes the expected execution mode; it is not a
readiness state. An Issue may carry `ready-for-agent` only when every item below
is satisfied:

- [ ] The governing English ADR/OpenSpec is accepted, or the Issue proves no
      new authority, public/schema, semantic, security, or migration decision
      is required.
- [ ] Exact upstream Issue and document sections are linked, with separate
      Start, Integration, and Completion dependencies.
- [ ] Owner, non-owner, input, output/commit, and persistence authorities are
      frozen, including deletion of any replaced authority without permanent
      wrappers.
- [ ] Public API, plugin ABI, IPC/API, document/artifact schema, versioning,
      and migration impact are explicitly decided.
- [ ] A named vertical fixture/corpus and an independent oracle or invariant
      are frozen.
- [ ] Unknown, unsupported, exhaustion, cancellation, stale, persistence, and
      rollback behavior is fail-closed or has an explicit bounded fallback.
- [ ] Exact durable tests, commands, expected artifacts, platform matrix,
      documentation, and review evidence are listed.
- [ ] Project fields and native hierarchy agree with the Issue body.

The portfolio dependency validator constructs the execution graph from Start
and Integration only and requires zero strongly connected components and zero
reciprocal dependency pairs. It validates Completion and
`Consumers / integration validation` separately as closure and consumer
evidence, never as execution edges.

If any item is missing, use one of the canonical roles `needs-triage`,
`needs-info`, or `ready-for-human`. Do not create a substitute role label.
Set Project `Verification` to `Blocked` when a required decision/fixture is
absent and to `Planned` when the specification is complete but evidence has not
yet been run.

## Completion gate

An implementation slice is complete only when:

1. every acceptance statement is backed by maintained product behavior tests;
2. success, failure, concurrency, cancellation, resource settlement,
   persistence, and package/platform boundaries are covered as applicable;
3. implementation matches the accepted authority and migration decision, with
   no duplicate legacy/new API;
4. English authoritative docs and faithful Chinese mirrors are coherent;
5. exact commands, environment, outputs, limitations, review threads, and
   remote integration evidence are recorded; and
6. the Issue, required descendants, dependency links, Project fields, and
   completion state all agree.

Consumers and cross-slice validation may supply completion evidence without
becoming reverse dependencies of the producer.

Numeric performance targets require representative baseline review before
acceptance. Closing a parent, Project, prototype, or planning task never by
itself promotes a target to current behavior.

## Planning-only changes

A planning-only change validates OpenSpec, Markdown/link/parity structure,
GitHub Issue/Project graph state, and both Git repositories. It records
`Not run: clean configure/full build/CTest; no runtime or source behavior
changed` instead of inventing runtime test evidence.
