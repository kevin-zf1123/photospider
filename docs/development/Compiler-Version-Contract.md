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

Package 0.20.0 removes 13 legacy format/color registry keys, including
`numeric.cast` and `numeric.encode_range`; see the
[retirement record](../built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md).
This is a breaking operation-surface change. C ABI, WorkflowDocument and trait
schema versions are unchanged. The installed consumer rejects a 0.19 package
request and verifies removed-key lookup, invocation and compilation failures.

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

## G4 implementation versions

The G4 development branch uses package 0.8.0, operation ABI/OperationTraits 8,
semantic-graph-ir-v8, physical-plan-v8, plan-cache-key-v8 and result-region-key-v4.
The five observation/failure/dependency fields and EffectiveAtomic participate in
the applicable identities. Optimizer-v5-canonical-noop, result digest v2, disk
framing v2, image facet v2, schema 2 and provider ABI 1 retain their meanings.
Version 7 DSOs are rejected before reading get_api_v8. No old aliases or image-v1
reader are provided. C++ consumers rebuild against 0.8. The prior foundations
paragraphs above describe their historical implementation boundary. C++ and C
staged programs execute through the existing Run and allocator, including exact
fragments, immutable dependency records, isolated shared observations and
content-result reuse. Native fragment atlases, bounded GPU discovery and CPU
fallback use the same protocol. See [Dependency data and execution](../kernel-architecture/Dependency-Data.md)
and the [G4 workflows](../../examples/g4_workflow/README.md) for implemented
contracts and executable acceptance cases.

## Independent result contracts

The #302 branch targets package 0.9.0 and operation ABI/traits 9 under ADR 0021.
M1 (#304) introduces ordered named output contracts and selected-output C/C++
invocations, with static metadata inference for all results. Repository singleton
callers now explicitly configure outputs[0]; ABI 8 is rejected before table access.
C v9 descriptors contain 1..64 active output records in a bounded inline table.
Semantic/physical-plan/plan-cache domains use v9 and result-region keys use v5;
complete output declarations, input projections and checked extent arithmetic
enter identities. Schema 2, provider ABI 1, C++17, image v2, conservative optimizer
v5 and result digest v2 remain. C++ consumers rebuild; 0.8 package consumers reject.
Multi-output planning, execution identities and Atomic joint execution have their
own later acceptance leaves; M1 does not claim those runtime gates complete.

## Phase A managed-resource foundation

Package 0.10.0 changes C++ execution configuration and host work-service
signatures. C++ consumers rebuild and package 0.9 consumers are rejected. The
resource foundation retains operation ABI/traits 9, document schema 2 and the
existing compiler identity domains; resource limits do not become semantic
inputs. Result/schema ABI changes are tracked separately in Phase A #316.
See [Managed resources](../kernel-architecture/Managed-Resources.md).

## Phase A structured C++ contracts

#316 adds OperationTraits 10, dependency protocol 2, compiler-visible result
schemas and owning paged ResultRefs in package 0.10.0. Semantic/physical-plan/
plan-cache domains advance to v10; result-region identity advances to v6.
Canonical schemas and Result port constraints enter those identities. Resource
limits and page sizes remain physical/admission choices. The unchanged optimizer
v5, WorkflowDocument schema 2, provider ABI 1 and result digest v2 remain.

The C operation DSO layout and v9 entrypoints are unchanged. Loading an ABI 9
Value/dependency plugin constructs current C++ traits internally. Structured
callbacks are currently registered through the installed C++ API; there is no
structured C descriptor table or compatibility shim. These version axes are
independent: C++ consumers must rebuild for 0.10, while unchanged ABI 9 C DSOs
remain loadable. See [Global results](../kernel-architecture/Global-Results.md).

#318 adds closed Layer schema version 1, the registered CPU Layer operation
factory, strict per-primitive arithmetic and `Status.reason` to the same 0.10
C++ package. The Status layout change requires C++ consumers to rebuild; C ABI 9
continues to project its existing error code and does not gain a C++ Status
field. Named working-space and arithmetic definitions are fixed by the schema
version and operation identity. See [Layer runtime](../kernel-architecture/Layer-Runtime.md).

#319 extends the same pending 0.10 C++ package with full `Status.detail`,
coordinate `AtomKey` joint contract 2, `execute_atoms` and opaque numerical
`QualityReport`. C ABI 9 keeps unique-output joint contract 1 and its unchanged
error-code projection. C++ callback outcome/supply signatures now use AtomKey;
consumers rebuild without an output-index shim. Joint contract 2 is a new
trait value in existing v10 canonical framing; contract 1 semantics and C
layouts remain. See [Atom errors and quality](../kernel-architecture/Atom-Errors-and-Quality.md).

## Numeric tuple and diagnostics contracts

Package 0.13.0 uses C++ OperationTraits 13, generic trailing-axis observation
grouping, numeric axis dtype inference and host-owned CPU numeric diagnostics.
Semantic, physical-plan and plan-cache domains use v13 because output
grouping, regional execution and view traits enter operation identity. Dependency protocol 2, joint contract 2,
result-region v6, WorkflowDocument schema 2 and the C operation ABI 9 remain.
C++ consumers rebuild against 0.13; requests for older minor packages are rejected.
The C ABI loader rejects dtype rules outside its existing v9 enum. Its layout
does not expose the new C++ grouping, dtype rule or diagnostics callbacks.

Kernel C/C++ builds require Clang, including Apple Clang. Correctness runs on
Ubuntu WSL use Clang too. The manual [numeric workflows](../../examples/numeric_workflow/README.md)
exercise the installed public API without adding integration-test registrations.

## NUM-09 layout implementation

Package version is 0.13.0 with C++ `OperationTraits` version 13.
The operation plugin ABI remains C ABI 9; its descriptor and entrypoint layout
are unchanged. The nine `array.reshape`, `array.transpose` and `array.slice`
keys use the existing C++ traits and dependency protocol and do not add a C ABI
field or compatibility alias. C++ installed consumers must rebuild for the
0.13 package.

Layout traits carry per-node shape/permutation/count metadata, regional atomic
execution where applicable, and `preserve_output_views`. Static dependency
pieces replace the earlier static dependency maps; each piece carries disjoint
observation coverage and complete per-port dependencies. These fields affect
the C++ operation identity. The layout operations set `cacheable=false`, since
the current content cache does not witness physical owner/stride partitions;
pure and active-Run sharing remain separate.

Static mapping identity includes each disjoint piece's full observation coverage
and every `DependencyAxis::translation`. Translations use signed Int64 values;
creation checks each translated piece with widened integer arithmetic before
execution. The C++ field is `static_dependency_pieces`; no old-field alias is
provided. Repeated input templates may set `repeated_match=false` only when a
metadata specializer supplies and validates their descriptor relation, as used
by concatenate's matching non-axis extents.

## NUM-12 pure block sharing

Package 0.14.0 uses C++ `OperationTraits` version 14 and semantic, physical-plan
and plan-cache domains v14. The C operation ABI remains C ABI 9. The opt-in
`share_blocks_across_outputs` trait is false by default and applies only to pure
Atomic dependency-v1 operations. Its common block identity includes all
resolved output contracts, static parameters, input metadata and supplied bytes,
incoming state, phase/range and mode. Public output and certificate identities
remain independent. Optional result-LRU reuse requires a positive
`result_cache_bytes` configuration and an accounted proof budget; a miss
recomputes, and the trait does not join concurrent producers or promise one
evaluation per Run. The C++ layout change requires consumers to rebuild against
0.14; older minor requests are rejected. WorkflowDocument schema 2, provider ABI
1, optimizer v5 and result-region v6 remain unchanged.

## NUM-01 static preparation and diagnostics

Package 0.15.0 keeps C++ `OperationTraits` version 14 and semantic framing
version 14; the C operation ABI remains C ABI 9. The public C++ operation
definition adds `prepare_static`, `OperationPreparation` and the immutable
`PreparedOperation` handle. Compiler nodes and plan steps may retain that
handle. Direct requests reuse an explicitly supplied matching handle or prepare
once per preflight; joint requests prepare once for compatible members. Matching
requires registry/definition identity, static metadata and copied IEEE-754
parameter bits. Separate calls do not share preparation implicitly.
Request records own their copied inputs while dependency queries remain
borrowed. Preparation and plan allocation are outside per-Atom runtime scratch
admission. No global cache or dynamic preparation state is introduced, and
prepared owners are destroyed after continuations and callbacks retire.

NUM-01's numeric diagnostics add `strict_math_calls` and the 8-by-4
`function_fallbacks` matrix. NUM-01 counts each strict math call; uninstrumented
operators contribute zero, and merge assigns unattributed reason counts to `Other`.
These are observations only. The prepared public manual, compiler and facility
checks passed the once/ROI/output/tile/foreign/signed-zero/NaN/lifetime paths;
NUM-01's public workflows and independent expression oracle passed on native
Clang and Clang WSL. C++ consumers must rebuild for 0.15 and older minor requests
are rejected.
The C ABI 9 descriptor/table layout remains unchanged.

## CRV-06 ColorArray and explicit resource bindings

Package 0.16.0 introduces a breaking C++ public layout and signature change for
ColorArray/ICC-backed values. C++ `OperationTraits` remains version 14 and the
semantic, physical-plan and plan-cache framing remains v14; WorkflowDocument
schema 2, provider ABI 1 and the C operation ABI 9 remain unchanged. The C ABI
descriptor and entrypoint layout do not carry these C++ resource bindings.

`IccProfile::import` admits validated immutable ICC v2/v4 bytes under an
explicit `ResourceBudget`; `ResourceBindings::create` seals the admitted
profiles and supports identity selection, references and set union. ICC identity
is content-based (SHA-256 plus length), not a path, address or ICC MD5 profile
ID. The handles are immutable and share accepted payload owners. Admission,
hashing, parsing, sorting and comparisons observe cancellation and work limits;
failure publishes no partial resource.

`Compiler::analyze` and `Compiler::compile` accept explicit `ResourceBindings`
and retain them in semantic IR, optimized IR and the execution plan. A
`ResourceBindings` set is also carried through `InputSnapshot`, `RegionalSource`,
`DependencyRequest`/`DependencyQuery`, `ResultProgramQuery`/`ResultContinuation`
and operation invocation. `Value`,
`ValueFragments` and `MutableValue::publish` accept and retain the validated
owners named by their facets; irrelevant bindings are dropped. Empty execution
paths still validate and retain required resources even when they read no
payload. Resource owners retire with the corresponding Value/fragment,
snapshot, plan or Run lifetime. Partial-channel ColorArray output requests
normalize to complete colors in plans, direct calls and retained demands; input
transport and fragments retain their complete-channel requirement.

These C++ layout, constructor and method signature changes require all C++
package consumers to rebuild against 0.16.0. No compatibility shim or older
minor reader is introduced. The recognized `photospider.color-array` facet now
has typed validation and complete-color demand semantics; earlier opaque-facet
treatment is not a compatibility contract. Persisted sample cache keys include
`PHOTOSPIDER_CACHE_BUILD_ID`, derived from kernel/operation/header sources and
build settings, so entries from the earlier implementation are isolated.
ColorArray itself is outside the existing disk-cache facet allowlist, and
resource-bearing results also bypass optional sample-only caches. Plans and
in-memory result caches have no cross-build deserialization path. Implementation
of CRV-06 and acceptance of its Proposed specification remain separate states.

## Package 0.17.0: prepared Whole execution

Package 0.17.0 changes the public C++ layouts of OperationInvocation and
OperationOutputSpecialization. Rebuild C++ consumers; 0.16 binaries are not
compatible. OperationTraits version 15 admits prepared CPU Whole callbacks,
static specialized input projections and CPU Whole Atomic tuples. The C
operation ABI remains 9: no C structure or entry point changes. Existing
projection/tuple fields already enter canonical identities; no prepared pointer
or new serialization field is added. Document and framing versions are unchanged.

### CPU Whole input views

Package 0.18 / OperationTraits16 extends CPU Whole view publication. A view
output prefers one original Value covering each complete input demand, retaining
its strides, storage and resources. If no covering Value exists, Auto may
collect; `requires_input_views=true` instead returns Domain/Run
InvalidArgument/InvalidDomain with ViewUnavailable before callback. It requires
CPU Whole `preserve_output_views`, excludes GPU/joint/Result and participates
in compiled identity. Typed validation still covers all active input samples.
The ordinary and structured execution bridges follow the same rule.

Explicit output payload bounds and on-demand view allocation apply to Whole.
Borrowed input owners remain charged independently; callback allocation is
limited to declared output payload plus workspace, with sticky failure. Returned
new backing storage must also fit the output bound. Direct calls already supply
one Value per input and preserve that physical representation.

This changes C++ trait/specialization layout, requiring an installed-consumer
rebuild and rejecting package0.17 consumers. Canonical framing14, document2,
C operation ABI9 and provider ABI1 are unchanged; traits16 changes semantic
identity. No daemon ownership or persistent format is introduced.

## Package 0.19.0: planar image storage

Package 0.19.0 introduces breaking C++ layouts for structural image storage,
workflow declarations, operation metadata/traits/callbacks and execution results.
Consumers rebuild against 0.19; requests for package 0.18 are rejected.
WorkflowDocument schema **3** rejects schema 2. The document remains a C++
compiler input, not a newly introduced persistent document format or dual reader.
OperationTraits is **17**. Semantic, physical-plan and plan-cache framing use
`semantic-graph-ir-v15`, `physical-plan-v15` and `plan-cache-key-v15`.
The optimizer rule remains `optimizer-v5-canonical-noop`. The C operation ABI
remains **9**, provider ABI **1**, and C++17 remains required; unchanged C tables
do not grant planar operation capability.

`WorkflowInputDeclaration.planar_layout` records explicit image axes, storage
mode, row pitch and component groups; its affine layout is empty. Planar
capability and output layout enter operation identity. Source declarations and
edge inference carry the structural layout. Per-DAG tile geometry remains a
planning choice included in physical identity. VM addresses, page owners,
budget identities and residency are excluded from semantic identity.

`PlanarImage` supplies the one CPU image-storage contract: one full-image virtual
reservation, on-demand page backing, planar row/tile addressing, exact valid
coverage and owner-retained read/write windows. Successful image outputs use
`ExecutionResult.images`, with explicit packed-region export. Generic numeric
Values retain their affine storage. Interleaved image input requires an explicit
import conversion; legacy image snapshots, Value fragments and callbacks do not
serve as compatibility implementations. Unsupported image operation/entry-point
capabilities fail explicitly and require subsequent migration or retirement.
No old numerical semantics are added as a fallback.

The public [storage contract](../kernel-specs/Tensor-Storage-and-Region-Access.md)
defines the supported CPU callback subset, resource/lifetime rules and runnable
workflow acceptance. The installed consumer exercises this structural workflow,
C SDK/header consumers and generic execution facilities. Older sections above
record their respective delivery contracts; they do not reinstate retired image
execution in package 0.19.
