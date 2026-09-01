# Region Semantics

The current package has no dirty-update API, ROI execution mode, dirty-source
lifecycle, or incremental propagation engine.

Every published `Value` carries one bounds-checked rank-general `Region`.
Compiler-visible `OperationTraits` carry one closed rule:

- `Whole`: complete logical coverage;
- `Elementwise`: input and output coordinates correspond directly;
- `Halo`: elementwise input demand plus a nonzero symmetric radius.

The compiler validates rule combinations and copies them into IR/plan identity.
The current executor still evaluates complete Values; it does not materialize a
partial Region plan. An operation callback must return a Value whose descriptor
matches the plan, whose Region covers the complete descriptor, and whose layout
passes ordinary Value validation.

Incremental dirty propagation is outside the active package boundary. Region
traits cannot create workers, storage, daemon state, or a claim that partial
execution exists.
