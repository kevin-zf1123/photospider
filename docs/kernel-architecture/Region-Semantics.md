# Logical regions and Result image samples

## Scope & Ownership

`Region` describes offset/extent intervals in a logical descriptor domain. `Footprint` represents exact sample coverage, including disjoint boxes and holes. Neither describes physical byte placement. `Tensor` owns immutable backing fragments; a Result and its captured descriptor bound the coverage available through a read capability.

## Data Layout & Memory

`ResultTensorSpec` contains a member key, dtype and cell shape, explicit `batch_axes`, `atomic_trailing_axes`, and optional spatial layout. Complete sample coordinates concatenate batch and cell axes. Spatial H/W and channel indices are cell-relative; axis order is explicit. No channel count implies a color or alpha interpretation.

`close_samples` closes a sample region over the trailing atomic cell axes. Tile authorization is a separate geometric step: spatial tiles cover all non-H/W cell axes, including channels, even when no axes are atomic. Nonspatial tiles split the last two non-atomic cell axes; earlier such axes and explicit batch axes form separate coordinates. Atomic axes remain complete.

All logical extents are positive. An empty Footprint selects no samples without changing schema. Data-dependent active counts are ordinary payload members inside a static-capacity schema. Padding, holes and uncommitted samples do not become readable. Owning read windows preserve actual negative, zero or positive strides and stop runs at physical and authorization boundaries.

## Selection and execution

Output selections name member slots and compact tile intervals. During compilation, an operation lowers the selected output tiles into a fixed graph of tasks with declared reads, writes and scratch. Complete-input algorithms declare every required input tile. Metadata-only reads require input schema without executing a payload producer. Empty payload selections omit pure output tasks; control-flow conditions and mandatory effects remain roots.

The Plan records physical dependencies before execution. Runtime control flow selects a compiled branch or repeats a compiled block. Numerical callbacks receive fixed views and cannot request additional producer work. Exact sample Footprints describe the coverage of external Results and read windows; task scheduling uses the Plan's tile and dependency tables.

## Tile projection arithmetic

For positive tile extent `T` and logical extent `E`, the tile count is `E / T + (E % T != 0)`. This avoids overflowing `E + T - 1`. The plan requires a power-of-two T, default 128. Products, tile indices and interval endpoints are checked. Selections keep complete tile ranges compact; lowering expands physical tasks under explicit compilation limits.

Logical grids are independent of backing geometry. Execution preflight requires each selected physical tile to fit its declared layout and allocation. A different tile grid can therefore require explicit COPY tasks to adapt existing backing.

## Limits & Error Handling

The data layer checks published regions, overlapping writes, affine address bounds and read-window authorization. Compilation and Run preflight check declared schemas, layouts and storage bounds. A trusted numerical kernel must honor every declared read and initialize every declared output sample before success; the executor does not inspect its sample accesses or numerical values. See [Global Results](Global-Results.md#execution-publication-and-lifetime) for Result ownership and ordered call exports.

A failed ResultBuilder transaction adds no coverage, while earlier published regions remain immutable. Cancellation drains accepted execution tasks before releasing their borrowed storage. Empty output selection preserves compiled control flow and mandatory effects on the executed path, as defined by [Compiler and execution](Compiler-and-Execution.md).
