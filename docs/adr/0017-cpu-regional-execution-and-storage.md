# ADR 0017: CPU Regional Execution and Storage

- Status: Accepted
- Date: 2026-09-09
- Acceptance: the maintainer explicitly requested implementation of the complete S2 plan in this task, including package 0.4 / operation ABI 4 and protected PR delivery.
- Baseline: `main@70b760fee96575391b825df1b179480407f33a2b`
- Decision Issue: #263; implementation: #264, #210, #211, #265, #266
- Reader mirror: [Chinese](zh/0017-cpu-regional-execution-and-storage.zh.md)

Acceptance specifies a target, not completed implementation. GitHub Issues
own delivery status. ADR 0015 retains product ownership and exclusions. This
ADR replaces ADR 0016's whole-only runtime storage/output, full-image numeric
preflight, operation ABI 3 and modeled callback-budget clauses. Its source
schema, immutable binding snapshots, image profile, scalar domains and
cancellation/currentness priority otherwise remain.

## Research and selected approach

At the baseline, `Value::create` checks addresses over the whole descriptor;
the operation host requires whole input/output Values; physical demand is
propagated but not materialized. `ExecutionRun` retains every intermediate
while releasing modeled resource leases immediately after each callback.
Those mechanisms cannot establish bounded regional execution.

[libvips evaluation](https://www.libvips.org/API/current/how-it-works.html)
demonstrates region-producing sources and sink-driven evaluation.
[Halide scheduling](https://halide-lang.org/docs/tutorial/lesson_08_scheduling_2.html)
separates algorithm meaning from computation/storage placement and describes
locality, redundant computation and parallelism tradeoffs. We adopt explicit
regional demand and completion-scoped storage in the existing compiler and
ExecutionContext, without a framework dependency or an unmeasured speed claim.

Whole-image cropping after execution cannot meet small-ROI memory acceptance.
Retaining ABI 3 alongside a second regional ABI adds a second output/lifetime
contract; the maintainer selected one breaking ABI. Process RSS accounting
would require unrelated allocation instrumentation; S2 bounds controlled
computation buffers instead. Native devices, cross-run caches, forward dirty
propagation and disk-derived data remain later decisions.

## Logical values and CPU storage

A Value separates the full logical descriptor, valid Region, storage view and
shared immutable owner. A view carries an explicit logical origin, byte offset
and signed strides. Address validation covers the valid Region, with checked
rank, containment, integer arithmetic, element tail and storage bounds. Existing
negative/broadcast layouts remain legal when proven in bounds. Publication is
atomic and copies share only immutable storage.

Host-controlled mutable allocations become immutable at publication. Writable
output cannot overlap an active input or another writable allocation. Read-only
views may share an owner. Reuse requires every using callback to have finished
and every retained Value/view reference to have retired. Fan-out and repeated
edges must not release early. A result owns its reservation until its last copy
is destroyed; accounting state can outlive ExecutionContext.

`ExecutionBindings` additionally accepts a regional source with the declaration's
exact descriptor and facets. The source fills a host-provided region buffer
synchronously. A Run owns its immutable source snapshot until all admitted
callbacks retire. All names, metadata and scalar intervals are validated before
operation entry. Pixel domains are validated when their demanded region is read;
unread pixels are not scanned. Complete dense Value bindings remain supported.
Source/sink callbacks must not retain borrowed writable/read-only pointers.

## Operations and public versions

Package version becomes 0.4.0; OperationTraits and operation C ABI become 4.
All maintained C structures/constants/entrypoints use `_v4` / `_V4`; ABI 3 is
rejected before table lookup with no adapter. C++ consumers rebuild. C++17,
WorkflowDocument schema 2 and provider ABI 1 remain. Data providers still define
semantic schemas rather than runtime storage or file codecs.

Both operation APIs expose ordered regional input views, explicit output
Region, host-managed output allocation and scratch. The host owns validation,
allocation failure and immutable publication. Only successful completion
publishes output. C pointer/count/size validation and exception fencing remain.
Operation workspace requirements have a checked computable upper bound used
before tile admission; operation-internal pixel buffers use the host allocator.
A trusted callback violating the allocation contract does not acquire a sandbox
guarantee. Optional callback GPU behavior remains distinct from native devices.

Static halo specialization is a copied trait contract, resolved from validated
node parameters during analysis; it never depends on per-run pixels/scalars.
Add a bounded Float32 mask input port: rank-two {H,W}, no facets, finite [0,1],
spatially matching the image. Scalar ports retain whole {1}; RGBA ports retain
all four channels and the exact ADR 0016 profile.

## Demand, scheduling and resource ownership

PlanningOptions carries positive tile height/width, default 128/128, and named
output Regions. ROI/tile edits replan optimized IR. Canonical physical identity
includes normalized demands and tile geometry. Trait additions, static halo
specialization and mask semantics enter semantic identity. Use domain-separated
v4 semantic, optimizer, physical-plan and plan-cache domains; source schema
remains 2. Run payloads, allocation addresses, timing and cancellation are absent.

Generate tiles lazily in named-output lexical order and spatial row/column
order. Derive each tile's upstream demands separately, merging fan-out within
that tile; overlapping halos between tiles may recompute. Halo expands/clips
against the complete logical image, never the tile boundary. Retiling gathers
only demanded coverage into accounted buffers. Partial-channel, empty and
out-of-bounds image requests fail planning.

Only deterministic, side-effect-free paths with legal regional rules may
recompute per tile. Whole operations create explicit complete materialization
boundaries; their full working set and retained results are budgeted. Failure
to fit returns ResourceExhausted instead of silently exceeding the budget.

The ExecutionContext aggregate `maximum_live_bytes` bounds actual controlled
allocation capacity: source reads, outputs, scratch, retained intermediates,
transfers and sink staging. Shared storage is charged once. Caller-preexisting
inputs and caller-created copies are reported separately; metadata, stacks and
process RSS are outside this bound. Reserved working-set bytes and actual
allocation peaks are distinct diagnostics. Idle reusable capacity remains
charged until freed.

Before scheduling a tile, reserve a conservative complete working-set peak.
Temporary contention reduces concurrency or waits for admitted work that can
finish with its existing reservation. A minimum working set that cannot fit
fails; no callback holds partial resources while waiting for the remainder.
Externally retained results cannot force an indefinite wait for caller action.
The worker count, maximum parallelism and aggregate waiting-task bounds remain
finite. Diagnostic aggregation is bounded by graph size, not total tile count.

## Collection, streaming and failure

`execute` collects exactly the requested Region for each named result, using
accounted storage; no request means whole output. A streaming execution entry
uses the same plan and bindings with synchronous ordered sink callbacks. The
sink's borrowed view expires on return. Backpressure bounds completed-but-not-
consumed tiles. Callers may copy data into their own separately owned storage.

Check cancellation then currentness before admission, after callback completion,
before each sink delivery, after the final sink callback and after final assembly before returning. Entry still rejects a
foreign/default/stale plan before bindings or cancellation. After valid entry,
Cancelled precedes Stale and ordinary failures. Sink failure stops future
delivery; all admitted work retires before return. Already consumed tiles cannot
be rolled back; only a final successful return validates the complete stream.
Streaming is not a durable commit or result-publication protocol.

Malformed names/parameters/source descriptors fail before invocation; demanded
bound pixel-domain failures are InvalidArgument. Wrong valid types/coverage are
TypeMismatch, invalid computed pixels are OperationFailed, checked byte overflow
or allocation exhaustion is ResourceExhausted. Source/sink exceptions are fenced
as execution failures. No failure publishes a partial collected result.

## Image vertical and oracle

`S2Image.RegionAndTiles` is:
foreground -> Gaussian -> exposure -> mask -> source-over(background).

- `image.gaussian_blur` requires static Int64 radius in [1,64] and Float64 sigma
  in [0.1,64], finite, with no defaults. Radius resolves the spatial halo. Use a
  normalized sampled Gaussian kernel, horizontal then vertical passes, clamp
  at the image edge, fixed traversal and Float32 rounding after each pass.
- `image.exposure_gain` retains ADR 0016's dynamic Float32 gain [0,16].
- `image.mask` scales premultiplied RGBA by independent Float32 {H,W} [0,1].
- `image.source_over` computes F + B * (1 - F.alpha), including output alpha,
  following the [source-over formula](https://www.w3.org/TR/compositing-1/#porterduffcompositingoperators_srcover).

Preserve finite/HDR/premultiplied rules, nearest ties-to-even and gradual
underflow. Disable arithmetic reassociation and FMA contraction. Static kernel
coefficients use normalized binary64 arithmetic. Taps run from -radius to +radius; multiply and accumulate each tap in binary64 with no contraction, then each separable pass writes
binary32. Compare whole/tiled runs of the same implementation bit-for-bit.
An independent whole-image oracle uses abs(error) <= 1e-6 + 1e-5*abs(reference).
Include a hand-checkable small fixture, edges, nonzero ROI, non-divisible tiles,
tiles smaller than halo, masks 0/1, transparency, HDR and invalid parameters.

Programmatic large regional sources and a synchronous checking sink prove
bounded storage without retaining the complete image. Test fan-out, repeated
inputs, multiple outputs, concurrent Runs, slow/failing sinks, exactly sufficient
budget and one byte less, malformed views, cancellation/stale, cleanup and
result ownership after context destruction. Public built-in and installed C
plugin paths run the same example and independent oracle.

## Delivery

Order: #263 contract, #264 storage/ABI, #210 CPU liveness, #211 tile/halo,
#265 regional execution, #266 vertical, companion daemon installed-consumer
migration. #152 remains the broader parent; #209 machine calibration and native
storage/liveness/tiling scope linked to #153/#154 remain open. S2 does not close
HEX or MED parents. Issues record native dependencies, actual tests and merged
commits; Projects mirror them.

Use scoped independent code/contract review, affected static/shared installed
consumers and existing protected CI. Daemon migration only consumes the public
installed 0.4 package and adapts affected calls/codec without adding wire
features. Coordinate the breaking kernel and daemon PRs. No OpenSpec, feedback,
C++20, release archive or unrelated optimization is part of this decision.

## S3 target amendment

[ADR 0018](0018-local-result-caches-and-frozen-execution.md) explicitly permits bounded disposable disk-derived data and frozen execution, and replaces the operation/trait and scaled-region target. Its acceptance does not establish implementation completion.

## S4 target amendment

[ADR 0019](0019-metal-resident-image-workflows.md) adds explicitly selected Metal execution, native shared storage and operation ABI 6. Its acceptance defines a target, not implementation completion. All other boundaries remain.
