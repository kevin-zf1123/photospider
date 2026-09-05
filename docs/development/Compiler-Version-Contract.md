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
attestations, durable object ids, or receipts. Operation-v2 parameter schemas
and validated values affect semantic identity. Float64 parameters encode the
exact copied IEEE-754 binary64 bits in fixed little-endian order, so `+0.0`
and `-0.0` have different semantic, optimized, plan, and cache-key identities.
No NaN-payload, infinity, or signed-zero normalization is performed, and this
digest contract adds no finite-only validation. Plan-derived output/input
Regions affect physical plan identity.

## Cache compatibility

`PlanCacheKey` covers domain-separated plan identity. If an embedding creates a
derived compiler cache, it must validate schema, stage identity, operation
traits, and target capabilities before reuse. Any mismatch is a cache miss and
rebuild; cache deletion is always valid.

## Change checklist

- Update the affected public or internal version only.
- Update canonical digest vectors when canonical bytes intentionally change.
- Update affected English public documents and Chinese mirrors.
- Update live GitHub Issue/Project state and the checked-in delivery snapshot.
- Run focused stage validation plus the isolated installed consumer.
- Do not add a compatibility shim or second reader unless a separate explicit
  product decision requires it.

## Accepted S1 target versions, not yet implemented

[ADR 0016](../adr/0016-workflow-inputs-and-execution-bindings.md), following the
accepted direction, defines accepted target package 0.3.0, WorkflowDocument schema 2,
OperationTraits 3, v3 compiler identity domains and operation ABI 3. Provider
ABI remains 1 with Float32 element4. ABI3 publishes identical C/C++ per-port
constraints and rejects old operation ABI2 without an adapter. This concrete
contract is accepted; installed headers/runtime/build requirements have
not changed.

Evaluate C++20 separately. #257 checks static/shared installed consumers and
old-minor rejection. Demand-driven daemon features still require coordinated
breaking-package maintenance. Status writes follow [Task Collaboration](Task-Collaboration.md).
