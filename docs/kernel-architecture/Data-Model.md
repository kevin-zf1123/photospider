# Data Model

## 1. Scope and ownership

The compiler owns immutable workflow structure and resolved static schemas. Operations exchange `ResultRef` owners through Result ports. A Result has 1 to 16 named Tensor members. Their names, dtype, rank, cell shape, batch axes, atomic trailing axes and logical grids are fixed in the prepared plan. Structure may depend on input schemas and static parameters, never on runtime sample values. A changed input structure requires preparation and compilation again.

`Tensor` is the single numerical carrier. `TensorSpan` describes one rectangular backing mapping within it; it is not a second numerical model. `CpuStorage`, buffers, native allocations and resource leases represent physical resources. `ValueRef` remains a graph reference and `Result<T>` remains the error-return template.

Result and Tensor carry no attached color, alpha, transfer or LUT interpretation. Operations receive explicit parameters and resources where their algorithms need them. Ordinary image operations default to straight alpha; the kernel neither certifies this convention nor infers RGBA from four channels.

## 2. Core structures and memory layout

The public structures are in [`result.hpp`](../../include/photospider/data/result.hpp), [`tensor.hpp`](../../include/photospider/data/tensor.hpp) and [`tensor_span.hpp`](../../include/photospider/data/tensor_span.hpp). WorkflowDocument uses schema 7. `WorkflowInputDeclaration::result_schema` is required, and `ExecutionBinding` supplies an owning Result under its exact declared name.

The supported dtypes are UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64. Sample rank remains 1 through 8 and extents remain positive. A scalar uses `[1]`. No rank-zero, zero-extent, ragged or runtime-rank member is introduced. Sample shape concatenates explicit batch extents and cell shape. `InputAxis` and `InputElements` extent expressions are resolved from static input structure before execution.

A data-dependent number of records uses a static capacity, for example `records: Float32[N,R]` and `count: Int64[1]`, or same-shape data plus a mask. These are ordinary member names and payload values. Reading count or mask requires a payload Need and records dependencies. Every published tile is fully defined, including unused positions filled according to the operation contract. Missing tiles do not shorten the array. Capacity exhaustion fails unless the operation explicitly promises at most K results.

Host-created Result backing defaults to tiled planar storage. Logical grids and physical allocations are independent. Physical tiles and directories grow with produced coverage; neither a full logical canvas reservation nor a dense directory is required. Small tensors and edge tiles allocate for their actual coverage. One backing may serve plans with different logical tile extents.

`Tensor::create` validates descriptor, exact authorization and immutable fragments. Compatible overlaps deduplicate; missing coverage fails. Negative strides, zero-stride broadcast, multiple fragments sharing one owner and valid external backing retain their mappings. Structural checks cover dtype, rank, coordinates, byte spans, overflow, ownership and synchronization.

Operations acquire owning `ResultTensorReadWindow` objects and write through restricted `ResultTensorWriteWindow` callbacks. `row_run()` and `rectangle_run()` report actual addresses and strides and stop at authorization, physical tile and fragment boundaries. Retaining a window retains its backing. Borrowed callback services expire on return, including when poll yields.

Dense collection is explicit. `Tensor::collect` and SDK collection check coverage and capacity, charge copying and observe cancellation. Packing or transferring an unsupported device mapping is also explicit and charged. Complete logical coverage does not imply contiguous storage.

## 3. Scheduling and state

A Run fixes its plan, binding snapshot and generation. Every member has a logical grid under the plan-wide positive power-of-two `tile_extent`, default 128. Spatial members split H/W; nonspatial members split the last two non-atomic cell axes. Explicit batch coordinates stay separate, and atomic trailing axes remain complete. See [ADR 0023](../adr/0023-track-dependencies-between-fixed-tiles.md#31-member-tile-grids).

Payload Needs address member slots and compact tile intervals. Descriptor-only requests are explicit and grant no samples. An empty payload set does nothing. A continuation may request inputs across multiple polls. Ordinary output actors publish exactly one complete member tile; Run-shared stages provide global computation and may produce several tiles. See [Global Results](Global-Results.md).

Metadata closure includes static inputs needed for schemas and identity. Executable closure follows selected output projections and declared shared-stage dependencies. Metadata-only producers do not execute payload callbacks. Binding schema checks happen before callbacks, and the plan retains no caller sample addresses.

Workflow input ids and node ids occupy separate namespaces. At most 4096 input declarations have unique nonzero ids and unique 1-to-128-byte printable ASCII names. Result owners and read windows retain backing and Root leases after context retirement. External backing retained by execution is admitted under Referenced capacity.

## 4. Algorithms and validation

Logical schemas need not fit one dense allocation. Actual allocations and addressed byte spans must fit their representations and budgets. Tile counts, interval endpoints and indexing arithmetic are checked for overflow. Tensor storage preserves integer and floating-point bits; operations and explicitly bound validators impose any numerical domain constraints.

`SchemaTemplate::id` and `version` identify structural contracts and enter compilation identity. They do not select domain semantics inside the kernel. Domain reports such as LUT bake schemas belong to the ops SDK. Complete validators require authorized coverage of all members in their scope and do not automatically add input Needs.

## 5. Limitations and non-goals

Results have no durable serialization or recovery contract. Static schema and tile geometry do not imply dense residency. Batch axes carry no temporal meaning. The kernel provides neither dynamic field rows nor a separate whole-result execution mode. [ADR 0022](../adr/0022-freeze-operation-c-abi-v3.md) remains Proposed; its target ABI design is distinct from the implemented ABI 5.
