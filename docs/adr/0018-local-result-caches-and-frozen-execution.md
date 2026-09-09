# ADR 0018: Local Result Caches and Frozen Execution

- Status: Accepted
- Date: 2026-09-09
- Acceptance: the maintainer explicitly requested implementation of the complete S3 plan, including all choices below and per-Issue commits, independent review, protected PR delivery and cleanup.
- Baseline: kernel `54d57f30a68807c4d09fbc7439debda0a9b0c614`
- Reader mirror: [Chinese](zh/0018-local-result-caches-and-frozen-execution.zh.md)

Acceptance defines a target; the linked Issues record actual delivery. This
ADR replaces only ADR 0015's exclusion of local disk-derived Values, the
ordinary graph-currentness requirement for explicitly frozen execution, and
ADR 0017's operation/trait version and same-size spatial mapping clauses.
Application code owns preview requests, event queues and presentation. Kernel
objects own immutable inputs, execution, shared computations and disposable
caches. No daemon Job/IPC, document saving, recovery, artifact authority, native
GPU or incremental compiler product is added.

## Research and choice

[libvips evaluation](https://www.libvips.org/API/8.17/how-it-works.html) and
[tilecache](https://www.libvips.org/API/8.17/method.Image.tilecache.html) demonstrate
demand-driven regional production, bounded tile retention and downstream
invalidation. [OpenImageIO ImageCache](https://github.com/AcademySoftwareFoundation/OpenImageIO/blob/main/src/include/OpenImageIO/imagecache.h)
uses approximate retention limits; S2's controlled allocation limit remains
strict here. [libvips shrink](https://www.libvips.org/API/8.17/method.Image.shrink.html)
provides the box-filter comparison. No framework dependency or timing claim is
introduced. Existing FNV PlanCacheKey excludes payload and cannot identify
runtime results. #203 remains compiler-fragment reuse, not result caching.

Whole-image identity would invalidate unrelated work; caller-provided revision
claims would place correctness on unverifiable mutable data. Choose immutable
kernel-owned tiled snapshots and exact demanded content identities. A second
operation ABI path would duplicate regional validation; choose one breaking
ABI 5. Application request policy remains outside the kernel. Transactional
database recovery is unnecessary for disposable cache entries.

## Versions and scaled regions

Package 0.5.0, OperationTraits 5 and operation C ABI 5 replace 0.4/4. C++17,
WorkflowDocument schema 2 and provider ABI 1 remain. C++ consumers rebuild;
operation ABI 4 is rejected without adapters. Changed compiler semantics use
v5 digest domains. Runtime content identity uses separately domain-separated
SHA-256, preserving exact parameter/sample bit patterns including signed zero.

An explicit spatial box-shrink rule resolves a required integer parameter in
[1,16]. Output H/W are ceil(input H/W / factor); channels remain complete.
Backward demand multiplies output spatial coordinates by factor, clipped at
logical edges. Forward dirty mapping divides covered input coordinates with
floor/ceil rounding. All arithmetic is checked. Image and mask outputs retain
their respective profile. Edge cells average actual covered samples; box
accumulation uses fixed row/column order. Generic Whole remains conservative.

## Immutable input and execution snapshots

A public snapshot store imports valid Float32 RGBA images or masks, owns
immutable blocks and applies ordered exact-region replacement patches by
copying affected blocks only. It provides regional reads and identities.
Retained versions remain readable and share unchanged storage. The store has
an independent byte limit; failure leaves the original snapshot intact and
reports ResourceExhausted. Metadata is outside pixel-storage accounting.

An explicitly frozen execution snapshot captures a currently valid matching
plan, immutable bindings and registry ownership. It survives source graph
replacement/destruction. Ordinary execute retains existing stale/cancellation
priority. Frozen execution observes cancellation and its own validity, never
uses a later input version, and retains owners until callbacks retire.

## Result caching and shared computation

Caching is opt-in on ExecutionContext. Cache keys include exact local operation
semantics, parameter bits, ordered demanded input content and metadata, output
Region, backend and implementation identity. Global graph revision and
unrelated branches do not enter the local key. Generic unproven sources remain
executable but their dependent results cannot cross Run boundaries. Only
cacheable, deterministic, side-effect-free paths qualify. Scalar bindings are
copied immutable Values and use their exact bytes.

Forward invalidation respects Elementwise, Halo, shrink and Whole dependencies.
Changing exposure retains blur; changing local pixels affects only dependent
requested regions; unrelated graph edits preserve cache validity. Cache clear
and eviction always permit recomputation. Exact content identity, not a dirty
hint alone, authorizes reuse.

Identical in-flight computations share one producer within an ExecutionContext.
Every subscriber has independent cancellation. One cancellation cannot stop
another subscriber; the last subscriber requests producer cancellation.
Producer-owned frozen inputs and registry survive until admitted callbacks
retire. No callback waits on another callback occupying the same worker pool.
Only successfully validated immutable output can enter the completed cache.

Cache capacity is a sublimit of maximum_live_bytes. Shared allocations count
once; retained results continue owning their leases after eviction/context
teardown. Reclaim idle entries before admission; do not wait indefinitely for
caller-owned results. Failed cache admission skips retention; an impossible
working set fails ResourceExhausted. Report hits, misses, evictions, shared
computations, retained bytes and actual operation work separately.

## Operations and application workflow

Provide image/mask box shrink and image.brush_circle through C++ and the
maintained C module. Brush center, positive radius, nonnegative finite linear
RGB and alpha [0,1] vary per invocation. Pixel centers inside the closed circle
receive premultiplied source-over; outside pixels remain unchanged. One event
is one hard-edge stamp, with no automatic interpolation or device dynamics.
Apply only the clipped circle bounding Region, then publish its patch in order.

The application example uses a factor-four proxy followed by full resolution.
Proxy blur radius/sigma scale with resolution and clamp to supported bounds;
proxy output is explicitly approximate. Final/export uses original parameters.
Bounded queues coalesce pending slider previews; admitted brush events are
never dropped. Full admission reports backpressure and permits retry. Finite
tile batches alternate preview/export service. Publication checks content
version, target and quality; stale results and quality downgrades are rejected.
A frozen export continues reading its original input during editing.

## Disposable disk data

The embedding explicitly selects an exclusive local directory, byte and entry
limits. The kernel persists only finite CPU Float32 image/mask regions in a
versioned uncompressed canonical byte representation. Validate descriptor,
facets, Region, checked byte length, key and SHA-256 checksum before publication.
Persistent eligibility requires a verified maintained implementation fingerprint;
implementation/build changes cause misses. Unknown external implementations
remain eligible only for process-local reuse when otherwise proven pure.

Temporary files become entries only after completion. Incomplete, corrupt,
unknown-version and mismatched entries are discarded and recomputed. A bounded
asynchronous writer drops cache writes on failure or pressure, outside required
preview publication. Restart may reuse valid derived data but never restores
work state, requests, documents or output authority. No durable commit guarantee
is advertised. A single active owner uses a directory at a time.

## Acceptance and delivery

S3Cache.LocalInvalidation compares cached and uncached results and exact work
regions after exposure, stamp and unrelated-branch changes. S3Preview.LatestAndExport
replays edits while exporting a frozen snapshot, proving order, bounds, progress
and publication arbitration. S3Disk.DiscardAndRebuild uses separate processes,
corruption, deletion and eviction, and compares independent formal results.

Cover odd sizes, clipped edges, halo, transparent/HDR data, invalid parameters,
old snapshots, cancellation/clear races and exact/insufficient budgets with
deterministic synchronization. Ship examples/s3_image_workflow through installed
public APIs. Static/shared C/C++ consumers and daemon 0.5 consumption remain
required. Each leaf has a separate commit; comprehensive independent review and
required CI/bot fixes precede protected merge and Issue settlement.

## S4 target amendment

[ADR 0019](0019-metal-resident-image-workflows.md) adds explicitly selected Metal execution, native shared storage and operation ABI 6. Its acceptance defines a target, not implementation completion. All other boundaries remain.
