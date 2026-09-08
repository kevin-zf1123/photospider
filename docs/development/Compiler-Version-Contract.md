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
attestations, durable object ids, or receipts. Operation-v3 parameter schemas
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

## Implemented S1 versions

[ADR 0016](../adr/0016-workflow-inputs-and-execution-bindings.md) is implemented
by #257: package 0.3.0, WorkflowDocument schema 2, OperationTraits 3 and operation
ABI 3. Provider ABI remains 1 and adds Float32 element code 4. C++ consumers
must rebuild; schema 1 and operation ABI 2 are rejected without adapters.
`execute(plan, token, options)` becomes `execute(plan, {}, token, options)`.
SameMinorVersion rejects a 0.2 package consumer against 0.3.

The domains are `semantic-graph-ir-v3`, `optimizer-v3-canonical-noop`,
`physical-plan-v3` and `plan-cache-key-v3`. Canonical declarations encode id,
name, element, shape, Region, layout and sorted facet key/version/payload.
Ordered sources encode tag 1 for node/step and 2 for declaration. Port kinds and
binary32 interval endpoint bits enter traits; all integer fields use uint64
little-endian framing. Payload bytes and binding order never enter these stage
identities. No runtime result cache is introduced.

C++17 remains required. Static/shared installed consumers execute the named
image oracle through C++ and C plugin paths and verify old-minor rejection.
Daemon feature/wire work remains separate; its 0.2 package consumer needs a
coordinated migration before consuming 0.3. Status writes follow
[Task Collaboration](Task-Collaboration.md).

## S2 storage/ABI foundation

#264 implements package 0.4.0, OperationTraits 4 and operation ABI 4 with regional
storage views and host output/scratch allocation. Schema 2/provider ABI 1/C++17
remain. C++ consumers rebuild; ABI 3 and package 0.3 consumers are rejected.
Semantic/optimizer/physical-plan/cache domains now use v4. Runtime region-origin
addresses never enter semantic identity. The remaining S2 scheduler, resource
and vertical work is tracked by #210/#211/#265/#266 under ADR 0017.

#211 adds bounded numeric parameter records and static halo parameter names to
v4 semantic framing (bound flag, exact binary64 endpoint bits, length-framed
halo key). Physical v4 framing also includes tile height/width and each sorted
output name's exact Region. Aliased names with different Regions now have
different physical identities even when their merged producer demand agrees.
