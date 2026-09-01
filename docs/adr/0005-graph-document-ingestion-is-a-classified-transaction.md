# ADR 0005: Workflow Source Publication and Compilation Are Separate Atomic Steps

- Status: Accepted, revised by ADR 0015
- Date: 2026-09-01 boundary revision

## Context

The kernel receives a caller-owned, format-neutral `WorkflowDocument`.
Replacing source state and validating compiler semantics have different failure
boundaries: source replacement must be atomic, while an invalid graph must
fail without publishing partial IR or a plan.

## Decision

`GraphContext` copies a complete source document and owns a nonzero monotonic
revision. `snapshot()` captures one coherent document/revision pair.
`replace()` prepares the new copy before publication and then advances the
revision under the same lock. Allocation failure leaves source and revision
unchanged; revision overflow fails before publication. A successful
replacement immediately makes older snapshots, IR, and plans stale.

Source publication does not claim semantic validity. `Compiler::analyze`
validates a captured snapshot as one fail-before-publication transaction:

- document version, nonzero unique node ids, and unique nonempty output names;
- source references, ports, operation availability, and parameter vocabulary;
- cycle-free deterministic topology;
- operation input counts and statically inferred type/shape/Region rules;
- graph currentness before returning complete `SemanticGraphIR`.

Recoverable failures use the public `ErrorCode` categories such as
`InvalidArgument`, `NotFound`, `Cycle`, `TypeMismatch`, and `Stale`.
`std::bad_alloc` remains process resource exhaustion and is not converted into
a successful empty graph.

Later optimizer and planner stages construct complete immutable values before
return and repeat the currentness check. No partial IR or plan escapes a
failure.

## Boundary

`WorkflowDocument` is an in-memory compiler input. The kernel owns no file
discovery, parser, YAML adapter, document persistence, storage service, or
daemon wire error mapping. A consumer may translate a file or local IPC
payload into a document before calling the kernel.

## Consequences

- Source replacement is atomic even when the replacement is later found
  semantically invalid.
- Compiler validation never partially mutates a graph context.
- Revision checks reject plans compiled from replaced source.
- File-format policy remains outside the kernel package.
