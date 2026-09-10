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
attestations, durable object ids, or receipts. Operation-v4 parameter schemas
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
addresses never enter semantic identity. #210/#211/#265 implement completion-owned
allocation, lazy tile planning, regional sources and synchronous streaming under
ADR 0017; the Gaussian/mask/composition vertical is tracked by #266.
Result digest framing is `photospider.result-digest.v2`, including explicit storage origin;
streaming has no result digest because tiles are borrowed and retired.

#211 adds bounded numeric parameter records and static halo parameter names to
v4 semantic framing (bound flag, exact binary64 endpoint bits, length-framed
halo key). Physical v4 framing also includes tile height/width and each sorted
output name's exact Region. Aliased names with different Regions now have
different physical identities even when their merged producer demand agrees.

## S3 scaled contracts

#270 implements package 0.5.0, OperationTraits 5 and operation ABI 5. C++17, schema 2 and provider ABI 1 remain. Package 0.4 consumers and operation ABI 4 are rejected; C++ consumers rebuild. Integer box-shrink shape/Region rules support factors [1,16] and mask outputs. Static factor parameter names and resolved factors enter v5 compiler identities. Runtime content identity remains separate. See ADR 0018.

## S4 native contracts

Package 0.6.0 and operation ABI/traits 6 add synchronous pure C host GPU services,
native-backed CPU-accessible storage and explicit CpuExact/MetalFp32 planning.
Operation ABI 5 and package 0.5 are rejected. C++17, schema 2, provider ABI 1 and
daemon IPC v3 remain. Semantic encoding uses semantic-graph-ir-v6; physical and
plan-cache domains use v6 and include numeric mode and explicit access records.
The unchanged conservative optimization rule remains optimizer-v5-canonical-noop;
its digest changes through the new semantic input. Result region keys use v2
and separate numeric/backend/device/implementation identity. Native upload keys
hash actual logical bytes under their own domain. No native pointer or timing
enters compiler identity. See ADR 0019 and the installed S4 workflow guide.

## Accepted operation-foundations target

[ADR 0020](../adr/0020-composable-operation-foundations.md), tracked by #287,
targets package 0.7.0, operation ABI/OperationTraits 7, image facet v2 replacing
v1, semantic/physical-plan/plan-cache domains v7 and result-region-key v3.
The unchanged optimizer remains optimizer-v5-canonical-noop; result digest
framing remains v2. Disk-derived data becomes v2 with old entries treated as
misses. WorkflowDocument schema 2, provider ABI 1 and C++17 remain unchanged.
Complete constraints, output facets and inference rules enter the affected
identities. Old operation tables/package-minor requests are rejected; C++
consumers rebuild. Daemon 0.6 migration is a separate task.

These are accepted targets on `ops-foundations`, destined only for `ops`.
The decision baseline `main@fba06270` remains package 0.6.0/ABI 6; this document
change does not claim a 0.7 runtime or an implementation merge into main.

The shared-contract implementation in #289 now provides these 0.7.0/ABI7
interfaces on ops-foundations, including canonical typed facets and complete
constraint/dense-output identity. This remains local branch implementation;
#290–#297 and verified delivery into ops are separate completion gates. Main
remains at the decision baseline until independently authorized work changes it.
