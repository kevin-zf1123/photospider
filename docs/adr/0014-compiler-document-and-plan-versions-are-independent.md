# ADR 0014: Compiler Documents, IR, Plans, and Digests Have Separate Identities

- Status: Accepted, narrowed by ADR 0015
- Date: 2026-09-01 boundary revision

## Accepted target amendment by ADR 0016

The accepted target extends canonical identities with static input/port schemas and excludes ordinary scalar/image run bytes; the specified new domains and versions belong to implementation #257.
See [ADR 0016](0016-workflow-inputs-and-execution-bindings.md). Existing version
and representation descriptions below remain the implementation baseline until
#257 delivers the target; decision acceptance does not report runtime changes.

## Context

A typed compiler needs reproducible intermediate identities without confusing a
source document, semantic meaning, optimization result, physical plan, runtime
allocation, cache entry, or daemon object.

## Decision

The compiler pipeline has four explicit value domains:

1. `WorkflowDocument`, the caller-owned source model;
2. `SemanticGraphIR`, normalized and type/shape/trait validated;
3. `OptimizedGraphIR`, a semantically equivalent optimized form;
4. `ExecutionPlan`, a local physical plan for one target capability set.

Each stage is immutable after construction and is validated before the next
stage begins. Compiler diagnostics carry source locations and stage-local
codes; they do not mutate the input document.

### Version axes

Document schema, operation-trait schema, semantic IR schema, optimizer rule
set, physical planner, public package/API, and daemon IPC are independent
version axes. A change in one axis does not silently claim compatibility in
another. During 0.x development, an installed package/API change may be
breaking and must be tested through an isolated consumer.

Internal semantic/optimized/plan representations are not wire formats and are
not serialized by the daemon. No reader compatibility promise is made for
internal IR across releases.

Each in-memory stage carries a private weak identity for the exact frozen
operation registry that produced it. Optimizer, planner, and executor reject a
stage from another registry even when operation keys match. This runtime
freshness identity is excluded from canonical digests and wire/package data.

### Digests and cache keys

The compiler may expose:

- `SemanticGraphDigest` for normalized semantic content;
- `OptimizedGraphDigest` for the optimized form plus optimizer identity;
- `ExecutionPlanDigest` for physical plan content plus target capabilities;
- `PlanCacheKey` for derived lookup.

Canonical hashing uses explicit field ordering, widths, enum spellings, and
the exact IEEE-754 binary64 bits present in each copied Float64 parameter.
Positive and negative zero therefore remain distinct at semantic, optimized,
plan, and cache-key stages. The compiler does not normalize NaN payloads or
infinities and this identity rule adds no finite-only validation; every
schema-valid copied bit pattern is encoded in fixed little-endian order. A
digest excludes runtime allocation ids, addresses, timings, cancellation
observations, queue state, and daemon ids.

These digests are non-security identities for reproducibility, diagnostics,
benchmark comparison, and disposable derived caches. They are not signatures,
certificates, attestations, authorization tokens, durable object identities, or
receipts. Plan caches can always be deleted and rebuilt from source plus the
current operation traits and compiler.

### Correctness gates

Every stage checks duplicate node ids, missing references, cycles, operation
availability, the operation-published closed parameter vocabulary, required
items, exact parameter types, parameter bounds, type/shape/`Region` rules,
integer overflow, backend capability, and plan dependency ordering as
applicable. Unknown, missing, wrong-type, or conflicting parameter declarations
fail before semantic IR publication and no built-in callback supplies a hidden
default. An
embedding-provided cache hit must be revalidated before use. A malformed or
stale entry becomes a miss; it cannot bypass compiler validation.

Physical planning accepts optional bounded demands for named workflow outputs.
It propagates those demands backward as whole-input, elementwise-exact, or
overflow-safe clipped halo Regions, merges multiple consumers conservatively,
stores per-step output/input demands, and includes those values in physical
plan/cache identity. Execution verifies each produced Value covers the planned
input demand before transfer or callback entry. Complete Values remain the
current materialization boundary; the demand contract does not claim a dirty
or incremental executor.

### Closed source vocabulary

The current `WorkflowDocument` has no generic extension bag. Its closed fields
and parameter variant are validated directly. New semantic vocabulary requires
an explicit document/API version change plus compiler handling; unknown fields
are not silently accepted into IR or digests.

## Boundary

`WorkflowDocument` is compiler input, not a storage service. The compiler has no
durable migration authority, recovery journal, daemon lifecycle, or security
provenance role. ADR 0015 supersedes all broader meanings formerly attached to
these version and digest axes.

## Consequences

- Stage identities are inspectable and testable without becoming one global
  version number.
- Derived caches remain safe to discard.
- Daemon and package compatibility can evolve without exposing internal IR.
- Reproducibility digests do not imply trust or persistence.
