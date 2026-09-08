# Region Semantics

The current package has no dirty-update API, ROI execution mode, dirty-source
lifecycle, or incremental propagation engine.

Every published `Value` carries one bounds-checked rank-general `Region`.
Compiler-visible `OperationTraits` carry one closed rule:

- `Whole`: complete logical coverage;
- `Elementwise`: input and output coordinates correspond directly;
- `Halo`: elementwise input demand plus a nonzero symmetric radius.

Planning accepts optional bounded demands for named workflow outputs and walks
the plan backward. `Whole` demands every complete input. `Elementwise` maps the
exact output interval to each shape-compatible input. `Halo` expands that exact
demand symmetrically and clips it to the input shape without overflowing
`offset + extent + radius`. Multiple downstream demands merge to a conservative
bounding Region. Every output/input demand participates in physical plan and
cache identity.

The current executor still evaluates complete Values; it does not crop or
materialize a partial Value. Before transfer or callback entry it verifies the
available Value Region covers the plan-derived input demand and passes that
demand to the C++ callback/operation ABI v4 view. An operation callback must
return a Value whose descriptor matches the plan, whose Region covers the
complete descriptor, and whose layout passes ordinary Value validation.

Incremental dirty propagation is outside the active package boundary. Demand
legality does not create workers, storage, daemon state, or a claim that
partial execution exists.

## Per-port S1 demand

OperationTraits 4 supplies an ordered input schema. Value ports retain the
rules above. Float32Scalar ports always request whole {1}. Image ports require
Float32 {H,W,4} with the exact linear premultiplied profile; Elementwise maps
spatial H/W demand and Halo expands/clips only H/W. Every image demand includes
channels {offset=0, extent=4}. Partial-channel, empty, out-of-bounds and unknown
named demands fail planning, including partial-channel demands propagated from
a generic downstream consumer. Each step input is a tagged `PlanStepInput` or
`PlanWorkflowInput`; declaration references do not become scheduling tasks.

Changing `PlanningOptions.output_regions`, `tile_height` or `tile_width` replans
optimized IR. Tile dimensions must be positive and default to 128x128. Every
named output retains its exact Region in physical identity, including names
that alias one node with different requests. Runtime payloads remain excluded.

## Lazy tile planning

`ExecutionPlan::tile_plan(name, region)` derives a dependency-pruned plan for a
nonempty subset of that name's request without analyzing the source or building
a complete tile grid. Each tile walks demand backward independently, merging
fan-out only inside the tile. Overlapping halos may be recomputed. A step marked
whole_boundary (Whole rule, nondeterminism or side effects) receives complete
coverage and must execute once per Run; #265 owns runtime retention and streaming.
The ordinary whole executor remains the current integration path until #265.

Operation parameters may declare finite inclusive numeric bounds. Int64 bounds
must be exact integer endpoints within +/- (2^53-1); Float64 bounds are finite.
A halo_radius_parameter must name a required bounded Int64 schema with minimum
at least 1 and maximum no greater than UINT32_MAX, on a Halo operation with
zero fixed radius. Analyze validates source parameters and resolves that radius
into copied node traits before shape/demand inference. Unbounded Float64 source
parameters retain their previous exact bit-pattern behavior.

Float32Mask ports use rank-two {H,W}, no facets and finite [0,1] samples; H/W
must match the image. Their demand projects image spatial axes, while Float32
scalar demand remains whole {1}. Halo clipping uses logical image boundaries,
never tile boundaries. Propagated partial-channel image demand is rejected even
through a generic downstream port. Tile working-set bounds use regional output
bytes plus declared scratch. `test_tile_plan` covers static bounds, edge/clipped
halos, small tiles, fan-out, masks, Whole/effect boundaries and identity.
