# Compiler Version Contract

The public package, `WorkflowDocument`, operation-trait schema, semantic IR,
optimizer rule set, physical planner, and daemon IPC are independent version
axes. ADR 0014 defines their identity separation; ADR 0015 defines the product
boundary.

## Public compatibility

Photospider is in 0.x development. A minor release may make an explicit
breaking public API/package change. Every installed-boundary change must state
its impact and pass an isolated `find_package(Photospider)` consumer.

Internal semantic/optimized/plan representations are not public serialization
formats. The package does not promise that an internal IR from another build
can be decoded or executed. The daemon never places internal IR on local IPC.

## Digests

`SemanticGraphDigest`, `OptimizedGraphDigest`, `ExecutionPlanDigest`, and
`PlanCacheKey` use canonical domain-separated inputs. They exclude runtime
allocation, timing, cancellation, ready-queue state, and daemon identity.
They are non-security reproducibility/cache identities, not signatures,
attestations, durable object ids, or receipts.

## Cache compatibility

`PlanCacheKey` covers domain-separated plan identity. If an embedding creates a
derived compiler cache, it must validate schema, stage identity, operation
traits, and target capabilities before reuse. Any mismatch is a cache miss and
rebuild; cache deletion is always valid.

## Change checklist

- Update the affected public or internal version only.
- Update canonical digest vectors when canonical bytes intentionally change.
- Update English architecture/OpenSpec and Chinese mirrors.
- Run focused stage validation plus the isolated installed consumer.
- Do not add a compatibility shim or second reader unless a separate explicit
  product decision requires it.
