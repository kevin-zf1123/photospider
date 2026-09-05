# Refactor Development Plan: Accepted Direction

- Status: Accepted for direction and decision-preparation scope
- Acceptance: 2026-09-05; the maintainer explicitly accepted the recommended adjusted direction in the current task.
- Prepared: 2026-09-05
- Scope: planning and contract differences before resuming kernel #256
- Decision delivery: [#256](https://github.com/kevin-zf1123/photospider/issues/256); accepted ADR 0016 and synchronized public documentation
- Implementation baseline: unchanged; existing S0 settlement remains valid
- Reader mirror: [Chinese](zh/Refactor-Development-Plan.zh.md)

## Accepted direction

| Decision | Recommendation | Effect of acceptance |
| --- | --- | --- |
| Near-term delivery direction | Embedded image computation: S1 reusable images and ordinary parameters, S2 CPU regional execution, S3 result caching/interaction, S4 one native GPU backend, S5 measured optimization. Daemon feature work becomes demand-driven. | Revise stage priority and Issue scope while retaining task identities and actual dependencies. Daemon package compatibility remains maintained. |
| S1 image and parameter contract | Revise #256 for a bounded Float32 RGBA profile and ordinary numeric inputs that vary per run without recompiling. | Replace the earlier UInt8-only acceptance fixture; decide per-port input/Region rules and exact plugin compatibility before #257. Float32 is approved as an S1 target; the exact API/ABI contract has now been accepted in ADR 0016. |
| Future conflicting boundaries | Prepare narrowly scoped decisions for partial Value/Storage, regional invalidation, HP/RT, snapshot publication and disposable disk cache. | Authorizes decision preparation only. Existing accepted boundaries change only through explicitly accepted replacement clauses; it does not restore removed code or products. |

The maintainer's original requirements are flexible DAG-based image and alpha
composition, multiple image representations, GPU acceleration, large-image
and large-graph workloads, latency-sensitive painting and slider updates, and
retention of useful cache behavior. The referenced ChatGPT discussion and its
attached plan supplied proposals; acceptance comes from the maintainer
response recorded above.
This plan preserves the existing typed compiler stages and the separate daemon.

## Verified differences from the current plan and ADR candidate

| Current source | Difference needed for the recommendation | Task boundary |
| --- | --- | --- |
| The pre-adjustment ADR 0016 candidate used fixed scalar source parameters and an add/multiply UInt8 tensor fixture. | Separate ordinary numeric run inputs from static shape/halo/specialization facts; use an image fixture with changed pixels and changed numeric bindings. | Revise #256 before accepting its public API. #257 implements; #258 delivers the operation example. |
| PreserveFirstInput checks the first descriptor; MatchAllInputs requires every input descriptor to match. Elementwise/Halo require each input shape to match the output. | A rank-three RGBA image plus rank-one scalar cannot use the existing Elementwise/Halo path unchanged. Freeze explicit per-port descriptor, scalar and demand semantics. | #256 must address this before reporting that bindings alone satisfy the new S1 fixture. |
| Value and C operation/provider vocabulary only contain UInt8, Int64, Float64. The current C ABI validates exact structure sizes and closed enums. | Float32=4 needs Value/loader/trait/invocation/output/provider handling and a declared old-plugin policy. Equal C structure layout alone is insufficient evidence of semantic compatibility. | Version and consumer decision in #256; exact implementation in #257. |
| S1 draft requires complete dense CPU storage; current callbacks return complete Values. | Keep this as S1 support scope. Later regional/Storage work must replace the relevant contracts explicitly; no permanent all-storage-is-a-CPU-vector commitment. | S2 decision and implementation, not an automatic S1 general allocator. |
| Planning output Regions enter normalized physical demand identity. | Region changes can replan the existing optimized IR; ordinary numeric/pixel changes reuse the plan. | Keep current stage separation. Execute-time ROI is a separate choice, not a reason to reanalyze an unchanged semantic graph. |
| Cache-Model exposes PlanCacheKey and no cache service. #203 excludes retained execution Values and persistence. | Add result-cache/region-invalidation work separately; compile-fragment reuse cannot complete that capability. | S3-A and S3-C, after their decisions. |
| Region-Semantics excludes partial materialization/dirty propagation; GraphContext replacement invalidates plans. | Partial execution, fixed-snapshot export and latest-preview publication each need explicit changed contracts. | S2 and S3-B. Preserve current cancellation/currentness checks until replacements are implemented and tested. |
| ADR 0015 removes durable output/artifact/recovery mechanisms. | Decide a bounded local disposable disk-derived cache, owner, representation and validation. | S3-C boundary decision first; no direct restore of GraphCacheService or manifest-last machinery. |
| GPU currently means an optional callback lane with Run-local host-byte copies. | Native buffers/images, completion events, cross-run owners and budgets must be delivered and verified on actual hardware. | S4. A callback backend label does not establish native GPU residency. |
| Public CMake usage requirements and installed consumers explicitly require cxx_std_17. | Evaluate C++20 as an explicit toolchain/package task, including transitive usage requirements and C/C++ consumers. | A bounded S1 candidate task; no language-standard change in this planning turn. |

Evidence entrypoints: `include/photospider/plugin/operation_registry.hpp`,
`src/lib/compiler/compiler.cpp` (`infer_output_descriptor`, `derive_input_demand`),
`include/photospider/plugin/operation_plugin_api.h`,
`src/lib/plugin/data_definition_registry.cpp`, `CMakeLists.txt`,
`tests/consumer/CMakeLists.txt`, [Cache Model](../kernel-architecture/Cache-Model.md),
[Region Semantics](../kernel-architecture/Region-Semantics.md),
[ADR 0012](../adr/0012-operation-plugins-use-a-separately-versioned-pure-c-abi.md),
and [ADR 0015](../adr/0015-breaking-product-boundary-scope-reset.md).

## Stage acceptance and ordering

| Stage | Named acceptance scenario | Required observations | Start condition |
| --- | --- | --- | --- |
| S1 reusable image foundation | `S1Image.DynamicBindings`: two pointwise image operations, two image payloads and two ordinary parameter sets. | Compile once, run twice and concurrently; independent pixel oracle, unchanged compile count, no mixed bindings; explicit image profile and negative cases. | Accept revised #256, implement #257, deliver #258. |
| S2 CPU regions and bounded materialization | `S2Image.RegionAndTiles`: blur, exposure, independent mask and composition over an output Region. | Tiled output equals whole-image reference, including boundaries and non-divisible tiles; small requests avoid complete intermediates; bounded active tasks/storage, regional source and streaming sink. | S1 plus accepted partial-output, CPU Storage/lifetime and tile contracts. |
| S3-A HP result cache | `S3Cache.LocalInvalidation`: alter exposure, paint a local input area, or edit an unrelated branch. | Reuse unchanged blur results, invalidate only dependent demanded regions, preserve unrelated results; clearing the cache preserves correctness. | S2 plus accepted content identity, cache ownership and forward-invalidation contracts. |
| S3-B RT and requests | `S3Preview.LatestAndExport`: replay slider and brush events while a frozen export runs. | Bounded queues, useful progress, exact brush semantics, rejection of stale previews, no lower-quality overwrite at the same content version, correct fixed export snapshot. | Appropriate S2/S3-A primitives and accepted HP/RT, sharing/cancellation and publication contracts. |
| S3-C disk-derived cache | `S3Disk.DiscardAndRebuild`: reload valid data, then corrupt or evict it. | Validate finite supported representations, discard bad entries, recompute the same formal result; disk writes are outside the required preview-publication path. | Explicit local boundary/owner decision. May overlap S4; remains incomplete until delivered. |
| S4 native GPU | `S4Gpu.ResidentChain`: execute the same image chain on one chosen native backend. | Real device work, bounded staging, meaningful transfer counts, retained copies where valid, completion-safe recycling, numeric tolerance and CPU fallback. | S2 storage/lifetime foundation and accepted native device contract; no dependency on all of #203 or disk-cache completion. |
| S5 measured optimization | `S5Mixed.Loads`: large image, large locally edited graph, frequent slider/brush requests with background work. | Independent correctness, bounded memory/work, operation/transfer counts, separately measured compile/bind/queue/compute/transfer/validation/publication. | Usable preceding capabilities and measured evidence for each chosen optimization. |

Performance observations start in S1. CI checks deterministic behavior and
resource invariants. Hardware-dependent timing targets, p95/p99, backend choice
and scale are decided from recorded hardware/build/driver/sample conditions.
Without a real display path, measure request-to-result latency only. A global
operation such as a histogram must have a budgeted valid algorithm or explicit
resource failure; tileability is never inferred for every operation.

## S1 and S2 work to make executable

Retain #255 as the parent index and #256 -> #257 -> #258 as the ordered
contract/implementation/vertical leaf sequence.

| Existing or candidate task | Concrete proposed remaining work | Acceptance and stop condition |
| --- | --- | --- |
| #256 | Freeze static versus ordinary run inputs, bounded Float32 image profile, per-port descriptor/Region rules, ownership, identity, versioning and fixture. | Accepted bilingual ADR with exact API and negative fixtures; no implementation in this Issue. |
| #257 | Implement only that accepted contract, including all Value/trait/ABI/consumer changes it requires. | Direct positive, mismatch, concurrency, cancellation/stale and installed static/shared consumer checks. |
| #258 | Ship two real pointwise image operations and an installed example using changed images and ordinary numeric inputs. | Exact or bounded numeric oracle; compile count unchanged; two runs and concurrent isolation. Reuse #246 only for the starter portion directly needed here. |
| Candidate C++20 toolchain leaf | Inspect compiler/stdlib and supported consumer constraints; choose minimum versions and PUBLIC/INTERFACE usage requirements. | Accepted toolchain choice and focused public/C consumer validation plan before any flags or source migration. No all-class rewrite. |
| Candidate CPU Storage/partial-output decision under #152 | Define logical Value versus storage owner, region access, source/sink, completion lifetime, budget and failure ownership. | Precise English contract/mirror and fixture before regional implementation. Preserve C ABI and package version axes explicitly. |
| #210 CPU-focused revision | Model internal output/temporary liveness and safe reuse by actual completion, separating estimates, retained data and allocation. | Fan-out cannot recycle early; forked work stays within a declared budget; alias negatives and actual peak observations. |
| #211 CPU-focused revision | Define tile/halo/boundary/retile and global fallback for the representative image chain. | Concrete small-grid reference results, edges and non-divisible tile sizes; no full intermediate materialization on the regional path. |
| Candidate S2 implementation/vertical leaf | Implement the accepted CPU Storage/partial-output and #210/#211 semantics; include independent mask and alpha composition. | Regional input and streaming output, memory/task bounds, cancellation and full-reference equivalence. Split further only when actual ownership allows independent work. |

The candidates above have no allocated GitHub ids. The open-title inventory
and the selected detailed bodies show no matching C++20 or dedicated result/RT/
disk-cache implementation leaf; this is a bounded search, not proof that every
older discussion lacks overlap. Check candidate matches before creating one. This document records delivery scope,
not a second task-status tracker. S3-S5 remain outcome-level planning until
adjacent interfaces and decisions are ready.

For scalar inputs, a conservative Whole operation is a possible first
implementation: it reads the full image and the whole rank-one scalar. An
explicit per-port rule can instead apply Elementwise/Halo only to image ports
and Whole to scalar ports. Either choice must publish scalar type, shape,
finite/range validation, and failure ownership; neither follows automatically
from the current PreserveFirstInput rule. The recommended #256 revision
prepares the per-port contract and its ABI consequences for review. Adding fields to
the exact-sized C descriptor requires an explicit new ABI layout/version;
a separate capability-flag approach can keep the v2 layout only if the new
host rejects Float32 values for legacy plugins lacking that capability.
A plain enum addition or recompilation recommendation is insufficient. It does
not silently relax the existing Elementwise/Halo meaning.

## Existing Issue mapping and remote changes to prepare

This table is a proposed edit scope; no remote objects were changed. Live
Issues were read on 2026-09-05 and the listed selected tasks are open.

| Existing Issue(s) | Proposed action | Dependency/history treatment |
| --- | --- | --- |
| #255/#256/#257/#258 | Retain ids and order; add image/ordinary-parameter/concurrent-reuse acceptance. | Preserve the original baseline and discussion. Completion still requires actual delivery. |
| #148 | Specify the minimum input/demand/execution-scope explain fields required by S1/S2. | Broader explain work remains open. |
| #149 | Separate the necessary legal pure-branch demand pruning from optional later CSE/folding work. | Keep current #256/#148 prerequisites until approved edits; don't automatically remove native blocking relations. |
| #203 | Retain disposable incremental compilation after stable pass contracts. | Existing #256/#149 blocking remains; runtime result caching is separate. |
| #151/#152 | Keep the heterogeneous parent indexes; revise #152's narrative order so a CPU subset can precede native device cost work. | Current #152 narrative starts with native Storage, then #209, #210, #211; native blocked_by lists for #210/#211 are empty. Narrative prerequisites still matter. |
| #210/#211 | Replace their generic scope-only bodies with the exact CPU slice above. | Keep parent mapping; link future device-specific remaining scope so it is not counted as delivered. |
| #209/#153/#154/#155/#156 | Activate by the selected native chain and measured needs. | Simple cost units, observations and safety budgets needed earlier remain part of those earlier slices. |
| #157/#158/#159/#160/#162/#214/#218/#219 | Reuse the existing media program for color/alpha/channel/facet semantics and independent oracles. Limit S1 to the selected profile; retain broader media scope. | Do not create a parallel image-semantics program or claim all MED work complete from one fixture. |
| #246/#247/#248 | Activate only needed starter/provider/manifest scope. | No blanket cancellation or completion; remaining scope stays visible. |
| daemon #9/#10/#11/#12 | Propose demand-driven feature scheduling. | Keep native technical dependencies; #11's lack of a start dependency remains true. Scheduling deferral is separate from technical blockage and completion. |

The daemon still builds only against an isolated supported kernel package.
Its current CI consumes the matching kernel branch or kernel main and its
package declares the 0.2 minor. A breaking kernel 0.3 or plugin/API cut cannot
be combined with an assumption that daemon maintenance can simply stop:
prepare either the minimal coordinated consumer/package migration or a
separately approved maintained-version/CI selection policy. Adding per-Job
bindings and bulk transport may be deferred independently of this necessary
compatibility work. Neither option changes kernel ownership of its own CI.

Links: [kernel Issues](https://github.com/kevin-zf1123/photospider/issues),
[#256](https://github.com/kevin-zf1123/photospider/issues/256),
[#149](https://github.com/kevin-zf1123/photospider/issues/149),
[#203](https://github.com/kevin-zf1123/photospider/issues/203),
[#152](https://github.com/kevin-zf1123/photospider/issues/152),
[#210](https://github.com/kevin-zf1123/photospider/issues/210),
[#211](https://github.com/kevin-zf1123/photospider/issues/211),
[daemon #9](https://github.com/kevin-zf1123/photospider-daemon/issues/9).

For S1, ADR 0016 accepts linear-sRGB, premultiplied RGBA Float32 with a
fixed rank-three `{height,width,4}` descriptor. Exposure gain multiplies RGB
and preserves alpha; opacity then scales all four channels. ADR 0016 defines
finite/range rules, exact image facet bytes, alpha-zero handling, numeric
rounding/tolerance and output Region. This bounded target remains unimplemented;
broader color-management scope stays with #158/#159/#160/#214 and #218/#219.

## Clauses requiring later decisions

- S2: revise Value/Region/operation output and Storage ownership for actual
  partial materialization. Keep logical semantics, physical layout and storage
  separate; explicitly define source and sink memory in the budget.
- S3-A: introduce forward invalidation and result-cache ownership separately
  from backward input demand and compile-cache identity. HP means formal
  full-quality derived results; RT means temporary previews. These are proposed
  migrated semantics, not currently implemented cache services.
- S3-B: distinguish frozen export snapshot lifetime from latest-preview
  publication and shared cache-work lifetime. Do not delete stale checks as a
  substitute for this decision.
- S3-C: propose a limited replacement of conflicting ADR 0015 clauses for
  disposable local disk-derived data. Exclude work persistence, recovery,
  user-document saving, artifact authority and remote storage. Choose its owner
  explicitly; old GraphCacheService code is historical reference only.
- S4: define process-local cross-run residency owner, validity and budgets,
  plus device completion before resource reuse. A shared_ptr lifetime alone
  does not define completion of asynchronous native work.
- Toolchain/frameworks: C++20 requires explicit toolchain/consumer selection;
  complete MLIR/Halide adoption requires a bounded comparative experiment.
  Stable serialization of internal IR remains outside the current boundary.

## Current result and next action

The direction and ADR 0016's concrete Float32, scalar-port and operation ABI v3
contract are explicitly accepted. Local decision preparation is complete;
ADR 0016 governs exact target versions and rules. C++20 tooling and later-stage
boundaries retain their separate evaluation and are not implemented claims.

Decision delivery and settlement are tracked in #256. The next implementation
task is #257 after agreed delivery of the accepted contract and task
authorization; implementation has not started.
