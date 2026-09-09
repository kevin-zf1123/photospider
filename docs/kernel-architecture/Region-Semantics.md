# Region Semantics

A Value's bounds-checked Region is logical coverage in its complete descriptor,
independent of its origin-relative storage view. S2 provides CPU regional
execution. S3 adds explicit dirty mapping, immutable snapshots and opt-in result
caching under ADR 0018.

## Planning

Whole demands complete inputs. Elementwise maps matching coordinates. Halo
expands symmetric demand and clips at the complete image boundary with checked
arithmetic. RGBA ports require all four channels, scalars always demand whole
{1}, and Float32Mask {H,W} ports project the image's spatial axes. Empty,
out-of-bounds, unknown-name and partial-channel image demands fail planning,
including channel demand propagated through a generic downstream port.

PlanningOptions retains each output name's exact Region and positive
`tile_height`/`tile_width`, default 128x128. Changing these options replans
optimized IR. Named Regions and geometry enter physical identity even when
several names alias one node. Runtime bytes never enter plan identity.

`ExecutionPlan::tile_plan(name, region)` derives one dependency-pruned plan
without source analysis or a complete tile-grid allocation. Fan-out demand
merges inside that tile; neighboring tiles may recompute overlapping halo.
Whole, nondeterministic and side-effecting nodes are explicit whole_boundary
steps. Their materialization remains complete and must fit the resource budget.

Numeric parameter schemas can declare finite inclusive bounds. Int64 endpoints
are exact integers within +/- (2^53-1); Float64 endpoints are finite. A
halo_radius_parameter references a required bounded Int64 with minimum >=1 and
maximum <=UINT32_MAX on a Halo operation with zero fixed radius. Analyze
validates and resolves it into copied node traits. Unbounded Float64 parameters
retain their exact prior bit semantics.

## Execution and storage

Image paths, explicit regional demands and RegionalSource bindings use the
regional executor. Complete generic scalar/broadcast execution keeps the same
bounded worker executor. Ordinary execute returns each name's requested
coverage; image collection packs that Region while preserving the complete
logical descriptor. No request means complete output.

The Run materializes Whole/effect boundaries once in topological order, then
lazily processes output tiles in name/row/column order. Completed Whole results
are immutable Run-local values, not a cross-run cache. References expire when
no remaining output needs them. Tile processing is sequential at the outer
level; independent dependency-ready branches use the fixed worker pools.
Callbacks see exact input/output demands. Source reads and computation buffers
use the same context budget and owned worker queue.

Regular Value bindings remain complete dense snapshots. RegionalSource copies
metadata/callable per Run and fills host-provided packed region storage, with a
separately declared scratch limit. It must return exactly the written requested
Region. Bounded scalar parameter ports require ordinary Value bindings; other
regional inputs may use sources. Every binding name, descriptor/facet set and
scalar interval is validated before a source or operation callback. Pixel
content is checked only in the region consumed by a constrained port.

A source must support concurrent immutable reads and observe cooperative stop.
Source and operation callbacks must not synchronously reenter execution on the
same context's workers. All admitted callbacks retire before their borrowed
source/output storage is reused. No source codec or provider-ABI extension is
introduced.

## Streaming and resource observations

`execute_stream` invokes a required synchronous ExecutionSink with borrowed
ValueView objects. Views and pointers expire when the sink returns. A blocked
sink prevents the next tile's source read, bounding output staging. Sink calls
run on the execute caller thread. The caller may copy pixels into its own
separately owned memory.

Cancellation/currentness is checked before admission, at callback entry and
completion, before and after each sink call, and after final assembly. A sink
failure stops subsequent delivery and drains admitted work. Consumed tiles
cannot be revoked; only final success validates the complete stream. Collected
failure returns no partial ExecutionResult. Entry Stale precedes token/binding
validation; after entry Cancelled precedes Stale and ordinary errors.

The budget counts actual controlled source/output/scratch/intermediate/transfer/
collector capacity once per owner. A conservative complete working set is
reserved before work, and individual allocations retain their leases until the
last owner retires. Source buffers and intermediate slots clear after their
last reader. Caller-preexisting Values and source-owned external state, metadata,
thread stacks and process RSS are outside the controlled allocation bound.
`retained_input_bytes` reports distinct preexisting Value storage; opaque source
state is not measured. Foreign accounting domains are not mistaken for this
context's allocations.

Diagnostics aggregate operation count/elements/timing by node/backend, report
successful source reads/bytes, delivered tiles, active callbacks, actual peak
and planned reservation peak. Storage peaks include collector, Whole and tile
allocations in the same Run. Streaming has no result_digest; its sink checks
actual pixels. Collected digests include logical coverage and storage origins.

`test_tile_plan` checks demand legality and identity. `test_regional_execution`
executes a 5x7 ROI of a logical 64 GiB image without full allocation, verifies
9 ordered tiles and exact resource bounds, and covers concurrent snapshots,
backpressure, source/sink failures, cancellation/stale, Whole/effect single
execution and multiple outputs. Gaussian/composition acceptance is #266.

## S3 additions

Shrink shape/Region traits map ceil-divided spatial output to clipped integer input boxes. operation_dirty_region maps edits forward, including Halo expansion and Whole/scalar fallback. Immutable snapshot bindings preserve old versions. Explicit FrozenExecution replaces editable graph currentness with pinned validity; ordinary execute retains stale checks. See [Cache Model](Cache-Model.md) and [S3 Workflow](S3-Workflow.md).

Shared producer allocation peaks are reported separately in shared_peak_live_bytes; peak_live_bytes retains caller-local allocation meaning. The context budget charges shared owners once.
