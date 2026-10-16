# Logical regions and Result image samples

## Scope & Ownership

`Region` describes offset/extent intervals in a logical descriptor domain. `Footprint` represents exact sample coverage, including disjoint boxes and holes. Neither describes physical byte placement. `Tensor` owns immutable backing fragments; a Result and its captured descriptor bound the coverage available through a read capability.

## Data Layout & Memory

`ResultTensorSpec` contains a member key, dtype and cell shape, explicit `batch_axes`, `atomic_trailing_axes`, and optional spatial layout. Complete sample coordinates concatenate batch and cell axes. Spatial H/W and channel indices are cell-relative; axis order is explicit. No channel count implies a color or alpha interpretation.

`close_samples` closes a sample region over the trailing atomic cell axes. Tile authorization is a separate geometric step: spatial tiles cover all non-H/W cell axes, including channels, even when no axes are atomic. Nonspatial tiles split the last two non-atomic cell axes; earlier such axes and explicit batch axes form separate coordinates. Atomic axes remain complete.

All logical extents are positive. An empty Footprint selects no samples without changing schema. Data-dependent active counts are ordinary payload members inside a static-capacity schema. Padding, holes and uncommitted samples do not become readable. Owning read windows preserve actual negative, zero or positive strides and stop runs at physical and authorization boundaries.

## Demand, Execution & State

Payload requests name a member slot and compact tile intervals. Algorithms may first read control values through ordinary Needs and use them to choose later tile Needs. The kernel records all these payload dependencies without data/control role tags. Descriptor-only requests are explicit, grant no samples and do not start a payload producer. Empty payload sets create no dependency or grant.

An input validator requiring complete coverage runs only after the operation has requested the full authorized scope. It does not widen Needs. Complete-input algorithms use `sdk::to_whole`; global work can run once in a Run-shared stage. There is no second execution mode for complete coverage.

Dirty means potentially affected, not numerically different. Member tile records and frozen shared evidence determine invalidation. Additional committed production coverage is progress, not a content edit. Exact sample Footprints remain useful for windows and views, but dependency and invalidation granularity is a member tile.

## Tile projection arithmetic

For positive tile extent `T` and logical extent `E`, the tile count is `E / T + (E % T != 0)`. This avoids overflowing `E + T - 1`. The plan requires a power-of-two T, default 128. Products, tile indices and interval endpoints are checked. Complete coverage stays a compact interval; actual tasks expand in bounded batches.

Logical grids are independent of backing geometry. Reusing one backing in plans with different T values changes logical authorization without requiring repacking solely to match tile boundaries.

## Limits & Error Handling

Out-of-domain indices, unauthorized reads, overlapping incompatible writes and incomplete ordinary tile publications fail. An ordinary publication must be complete, exactly cover its selected member tile and leave other members empty. Shared stages may publish several complete tiles over time. Fixed bundle visibility can delay exposure and proactively schedule siblings; see [Global Results](Global-Results.md#publication-scopes).

Cancellation, resource exhaustion and stale generations preserve their error status. Failed transactions install neither partial payload nor incomplete dependency evidence. Earlier committed tiles remain immutable. Explicit empty output demand performs no payload work; mandatory effects follow the ordinary-execution versus explicit-DemandQuery rules in [Global Results](Global-Results.md#mandatory-effects-and-repeated-inputs).
