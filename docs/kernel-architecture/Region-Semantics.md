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
demand to the C++ callback/operation ABI v2 view. An operation callback must
return a Value whose descriptor matches the plan, whose Region covers the
complete descriptor, and whose layout passes ordinary Value validation.

Incremental dirty propagation is outside the active package boundary. Demand
legality does not create workers, storage, daemon state, or a claim that
partial execution exists.

## Accepted S1 target, implementation pending

The development direction and Float32 goal are accepted.
[ADR 0016](../adr/0016-workflow-inputs-and-execution-bindings.md) specifies the
revised image/scalar/per-port design and operation ABI v3, now Accepted.
Implementation facts above remain unchanged; new elements and bindings are
not implemented. #256 tracks decision delivery; #257 owns implementation.
