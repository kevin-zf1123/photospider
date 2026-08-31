# ADR 0014: Compiler, Document, and Plan Versions Are Independent

- Status: Accepted as a target contract; not yet implemented
- Date: 2026-08-31
- Related: ADR 0006, ADR 0008, ADR 0012, Issues #196, #199-#203, #245

## Context

The current kernel uses unversioned `GraphDefinition` values through a YAML
adapter, request-local `ComputePlan`, a full-task-graph cache key, independent
operation implementation identities, Value/artifact digests, and graph-cache
manifest v2. `WorkflowDocument`, `OperationSemanticTraits`,
`SemanticGraphIR`, `OptimizedGraphIR`, compiler `ExecutionPlan`, compiler
digests, and a typed plan cache do not yet exist.

Treating any existing token as a shared compiler version would couple source
syntax, semantic interpretation, optimization, planning, executable output,
hashing, and cache storage. It would also let package or IPC compatibility
silently admit internal compiler bytes. The typed-compiler sequence therefore
needs a version and migration decision before #199 and #200 define fields.

The decision must preserve established boundaries: operation ABI v1 remains an
exact-size pure-C contract, daemon IPC v2 remains a separate repository-owned
wire protocol, current architecture documents continue to describe only
implemented behavior, and derived compiler artifacts can be rebuilt instead of
translated across incompatible semantics.

## Decision

### Every compiler contract has its own identity

The canonical-byte profile, `WorkflowDocument`,
`OperationSemanticTraits`, `SemanticGraphIR`, `OptimizedGraphIR`, planner
behavior, `ExecutionPlan`, three digest domains, plan-cache key, plan-cache
record, and compiler-extension envelope use independent identities and begin at
`1.0`.

Version compatibility is a directed allowlist. K1 admits only exact
`1.0 -> 1.0`. Equal major versions do not imply compatibility. Unknown or
unsupported identity, version, canonical profile, digest domain, algorithm, or
required extension fails closed. Writers emit only the current version and do
not downgrade.

The complete registry and compatibility matrix are maintained in the
[Compiler Version Contract](../development/Compiler-Version-Contract.md).

### Canonical identity is binary, framed, and domain-separated

Compiler Canonical Encoding v1 uses a deterministic big-endian `PSCC`
envelope plus a typed value tree. It does not hash YAML/JSON spelling, C++
memory, host order, padding, pointers, or iteration order.

`SemanticGraphDigest`, `OptimizedGraphDigest`, `ExecutionPlanDigest`, and
`PlanCacheKey` use SHA-256 over length-framed `PSDG` preimages with independent
domain identities and versions. A raw 32-byte hash without its typed domain is
not an interchangeable compiler identity.

### Migration follows authority

Durable source documents and external trait sidecars may have explicit,
bounded, deterministic one-way migration into the current version. The current
writer becomes the sole durable authority; no compatibility alias, parallel
writer, or automatic downgrade remains.

Compiler IRs, plans, digests, plan keys, and cache records are derived. An
incompatible version invalidates and rebuilds them from the earliest valid
current source. They are never migrated, reinterpreted, or executed stale.

The legacy GraphDefinition/YAML importer remains #200 work. The temporary
legacy/new planner differential belongs to #201/#202 and must end in one K4
authority cut.

### Plan-cache identity includes every plan influence

The typed plan key includes canonical/digest/planner/trait/IR/plan versions,
semantic and optimized digests, effective traits, operation implementation and
package identities, required semantic/planning extensions, pass-pipeline and
plan-affecting options/static inputs, and target capability facts.

It excludes source formatting and diagnostic metadata, Graph revision alone,
request/Run/session/time/trace/cancellation/queue observations, non-shaping
dynamic payloads, cache paths/eviction, persistence receipts, daemon state,
and runtime pointers/allocations/fences/leases. If an excluded fact is later
shown to alter plan bytes, reuse is invalid until that fact is versioned,
included, and the cache namespace is changed.

### Extensions participate by effect without crossing repository boundaries

Every compiler extension carries owner/name/version, effect, canonical codec,
and canonical payload. Required semantic/planning extensions enter applicable
canonical objects, digests, and plan keys; an unknown required extension
rejects. Optional diagnostic extensions may remain opaque in the source
document but do not become interpreted IR or plan identity.

Compiler objects remain kernel-owned. Operation traits use a future
engine-owned registry or separately versioned sidecar and do not change
operation ABI v1. Internal documents, IRs, plans, digests, cache records, and
extensions do not become IPC v2 or daemon schemas.

## Consequences

### Positive

- A source, semantic, optimizer, planner, hash, cache, or extension change can
  invalidate only the contracts that depend on it.
- Unknown and stale inputs fail before interpretation, cache reuse, scheduling,
  or execution.
- Canonical bytes and digest domains can be reproduced independently of file
  adapters, C++ layout, and machine architecture.
- One-way durable migration and derived rebuild avoid permanent dual document
  or planner authority.
- Traits can evolve independently while the installed operation ABI v1 remains
  byte-for-byte unchanged.

### Negative and mitigations

- Exact-only initial compatibility creates conservative rejection and cache
  misses. A broader edge requires explicit reviewed evidence rather than an
  inferred same-major rule.
- The canonical envelope precedes concrete schemas. #199/#200/#201/#202 must
  freeze field projection and bounds without silently changing the envelope;
  an encoding change requires a profile-version bump.
- Plan-key correctness depends on classifying every plan influence. Any newly
  discovered influence is a contract bug that requires invalidation and a
  regression before reuse.
- Diagnostic extensions are not preserved by internal IR. Their durable
  preservation owner remains the source document.

## Rejected Alternatives

### Use one compiler-wide version

Rejected because document, traits, semantic IR, optimized IR, planner,
execution plan, digest, and cache record have different compatibility and
rebuild lifetimes.

### Infer same-major compatibility

Rejected because these are pre-1.0 compiler contracts and an older reader
cannot know that an added field or extension is nonsemantic.

### Canonicalize YAML or JSON text

Rejected because adapter, number, duplicate-key, Unicode, whitespace, and
ordering behavior would become identity authority.

### Migrate cached plans across breaking versions

Rejected because rebuilding from current source is safer than translating
executable decisions across changed semantic or planner contracts.

### Append traits to operation ABI v1

Rejected because ABI v1 has exact record sizes, suites, symbols, padding rules,
and entry points. Compiler metadata has a separate version lifetime.

### Expose internal IR through daemon IPC

Rejected because IPC v2 compatible-maintenance and compiler ownership are
independent repository contracts. A later external view requires its own
projection schema.

## Relationship to current facts and delivery status

The maintained
[kernel architecture index](../kernel-architecture/README.md) records that all
typed-compiler objects remain unimplemented. Current graph, planning, cache,
and plugin facts remain in
[Graph Lifecycle](../kernel-architecture/Graph-Lifecycle.md),
[Compute Flow](../kernel-architecture/Compute-Flow.md),
[Cache Model](../kernel-architecture/Cache-Model.md), and
[Plugin ABI](../kernel-architecture/Plugin-ABI.md).

[Roadmap v3](../roadmap/Next-Stage-Execution-Plan.md) is the delivery-order
authority. The live Issue and Project items remain the status authority. This
accepted ADR freezes a target contract; it does not promote
`WorkflowDocument`, compiler IR, digests, `ExecutionPlan`, or plan cache into
current runtime behavior.
