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
demand to the C++ callback/operation ABI v3 view. An operation callback must
return a Value whose descriptor matches the plan, whose Region covers the
complete descriptor, and whose layout passes ordinary Value validation.

Incremental dirty propagation is outside the active package boundary. Demand
legality does not create workers, storage, daemon state, or a claim that
partial execution exists.

## Per-port S1 demand

OperationTraits 3 supplies an ordered input schema. Value ports retain the
rules above. Float32Scalar ports always request whole {1}. Image ports require
Float32 {H,W,4} with the exact linear premultiplied profile; Elementwise maps
spatial H/W demand and Halo expands/clips only H/W. Every image demand includes
channels {offset=0, extent=4}. Partial-channel, empty, out-of-bounds and unknown
named demands fail planning, including partial-channel demands propagated from
a generic downstream consumer. Each step input is a tagged `PlanStepInput` or
`PlanWorkflowInput`; declaration references do not become scheduling tasks.

Changing `PlanningOptions.output_regions` replans optimized IR. Named outputs
remain document facts. Merged normalized per-step demands determine physical
identity; a smaller request hidden by another whole-output name does not force
an identity change. All outputs remain complete whole Values.
