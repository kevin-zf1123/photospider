---
spec_schema_version: 1
id: KERNEL-tensor-storage
kind: shared_kernel_contract
status: Accepted
implementation_status: implemented_cpu
---

# Tensor storage and region access

This is the implemented storage contract for static Result Tensor members. See the [Chinese mirror](zh/Tensor-Storage-and-Region-Access.zh.md), [Data Model](../kernel-architecture/Data-Model.md) and [Global Results](../kernel-architecture/Global-Results.md).

## Scope and ownership

`SchemaTemplate::tensors` contains 1..16 fixed named members. `TensorDescriptor` supplies dtype and positive cell extents; `batch_axes` prepends explicit batch extents. Complete sample rank is 1..8. `atomic_trailing_axes` groups trailing cell axes into observations. Optional spatial topology names cell-relative H/W and channel axes. H/W cannot overlap atomic axes.

The single numerical representation is `Tensor`, composed of owned rectangular `TensorSpan` mappings. Buffers, `CpuStorage`, native storage and resource leases supply physical backing. Result and owning windows retain the backing they need. Neither shape nor schema id certifies color or alpha semantics.

### Metadata-derived Result outputs in C++ registrations

Preparation resolves each output schema from static input schemas and parameters. Extents can be fixed or derived from input axes or element counts. Runtime payload values cannot change structure. C ABI 6 metadata resolution can supply output schemas and static shared-stage declarations; it cannot preserve an operation-owned arbitrary prepared-state pointer. See [Plugin ABI](../kernel-architecture/Plugin-ABI.md).

## Core data model and memory

Logical shape, logical tile grid and physical allocation are separate. The schema need not have a dense element product that fits one allocation. Actual addressed byte spans, tile counts, interval endpoints and allocation sizes use checked arithmetic. There is no field storage or runtime extent protocol. Variable active counts use fixed-capacity tensors and ordinary payload count/mask members.

Host-created backing defaults to tiled planar. The backing directory contains produced fragments rather than entries for the complete logical domain. No complete-canvas virtual-address reservation is required. `ResultBuilder::start()` does not allocate all logical samples.

### Affine backing

A `StridedLayout` has signed byte strides and an origin. Positive, negative and zero strides are valid when every addressed element fits retained storage. A singleton axis contributes no span. Zero stride supports very large logical broadcast domains over small backing. Padding bytes are never logical samples.

A row run's `data` points to the first logical sample. `sample_stride_bytes` may be negative or zero; `bytes` describes its checked addressed span rather than a forward contiguous copy range. Rectangle runs additionally supply a signed row stride. Always consume actual run metadata.

### Spatial backing

Spatial topology controls logical H/W run axes independently of byte order. Default writes partition the requested Footprint at batch and physical tile boundaries, then expose bounded planar write windows. Nonspatial vectors, matrices and higher-rank tensors use the same backing and window machinery. Explicit batch coordinates and atomic trailing axes retain their structural meaning.

One publication can allocate a buffer shared by several chunk spans; a tile need not own a separate allocation. Compatible external affine or fragmented backing remains valid. A logical plan grid may use a different tile extent from its input backing.

## Tile layout and checked arithmetic

The plan-wide `tile_extent` is a positive power of two, default 128. Spatial grids split H/W. Nonspatial grids split the last two non-atomic cell axes; earlier non-atomic cell axes and explicit batch axes form batch coordinates. With one divisible axis, each tile has at most T observations. With none, each batch coordinate has one tile.

Default materialization uses actual chunk width and height for strides and payload size. It does not pad every edge row to T or align every small tensor to an OS page. Actual allocation capacity is admitted separately from logical coverage. Every multiplicative size and address operation is checked.

### Row-padded edge example

For a Float32 spatial edge chunk of height 2, width 72 and four planar channels, sample payload is `2 * 72 * 4 * 4 = 2304` bytes. Its tight planar row stride is `72 * 4 = 288` bytes. No 128-wide padded row or 128-by-128 full tile is required. External backing may have padding; read windows expose its real stride and never authorize padding as samples.

## Read windows and transactional writes

`ResultRef::acquire_tensor` returns an owning, move-only read window limited by the captured descriptor and requested rectangle. Later production cannot widen that captured authorization. `row_run()` and `rectangle_run()` truncate at authorization, physical tile and fragment boundaries. Spatial runs follow width/height; nonspatial runs use the final/penultimate axes. Default materialized chunks stop at batch boundaries. General external spans may cover several batches; callers follow sample_axis(), row_axis() and the actual returned range, which can traverse a batch axis for a nonspatial rectangle.

The window retains source ownership and cancellation state. Run lookup can return Cancelled after token cancellation; previously returned pointers remain owned until window destruction. Window acquisition never executes an upstream producer or fills a hole. Work for acquisition and access is charged through the applicable Root and limits.

`publish_tensor_kernel()` admits capacity and prepares private writes before invoking the callback. Region and Footprint overloads use the same Tensor machinery. The callback receives only requested unpublished coverage; all parallel writes must be disjoint and finish before return. Borrowed write windows expire on callback return. Successful validation and cancellation checks commit the prepared backing; failure exposes no new coverage.

Same-producer mutation or sealing during an active writer is rejected. Moving or replacing its builder fails the transaction. Previously committed samples remain immutable. Empty coverage does not invoke the writer or allocate sample payload. The ordinary executor additionally requires one complete selected member tile per publication; shared-stage commits may contain multiple complete tiles.

## Mapping views

`publish_tensor_view()` retains an authorized source window and its owner instead of copying samples when the mapping is representable. Axis transforms and reshape use explicit `ResultTensorViewTransform` rules. Non-reshape transforms map each source fragment independently, allowing different owners and strides. Reshape requires the fragments to join into one valid affine map. Singleton, signed and broadcast strides still undergo validation. Invalid mappings, holes, overflow and source-to-target ownership cycles fail.

Views do not create input authorization. There is no coordinator policy that silently obtains complete inputs or copies them on behalf of an operation. SDK `to_whole` emits ordinary tile Needs and returns owning capabilities; `to_tile` publishes compatible retained views. If a mapping requires a copy, the operation explicitly chooses and budgets collection or packing.

## Limits and error handling

Dense `Tensor::collect` checks complete authorized coverage before allocation. Packed copies and general strided copies both honor work limits and cancellation. Native packing/upload and download have explicit capacity, work, owner and synchronization checks. A zero-dispatch native alias still needs completed source writes and visible data.

Kernel callback guards save and restore floating-point state without imposing a numerical mode. Ops SDK guards establish the algorithm's required mode on each actual execution thread, including workers, preparation, validators and polls, and restore it at callback exit or suspension. No guard spans polls.

## Non-goals

This contract does not add rank-zero, zero extent, ragged members, dynamic rank, hidden full-array copying, semantic layout certification, automatic alpha association, producer replay on eviction, or durable backing. GPU execution evidence remains backend- and device-specific.
