# 01-numeric implementation work

All registered numeric definitions lower to physical operation programs and
prepare fixed invocation ABI 8 kernels or explicit COPY tasks. The module
interface is ABI 8. Each family fixes its input reads, selected output tiles,
scratch and task dependencies before a Run starts. Shared computation uses
tasks and private values within the call.

Complete input support is an algorithmic read requirement. It does not by
itself require allocating or computing every output tile: each fixed family
declares the output closure it actually needs. Empty selections omit pure
output work while preserving control flow and mandatory effects. A loop
backedge starts a new call activation with independent working storage.

The fifteen LUT3D baking geometry keys emit tasks for selected output tiles.
The four structured LUT3D keys also use fixed CPU invocations. Pack, Unpack
and Gate copy selected samples into independent dense tiles. Measure waits for
the complete owned table and reads all validation point/reference/applied
colors, then writes its thirteen report members with fixed exact-integer
scratch. The report records the actual owned-table ObjectId. Gate checks
configuration, table identity and tolerance in that order. An ordered
retirement observer may retain a successful report when a later Gate fails.

Authoring templates expand ordinary workflow nodes with independent output
identities. Their output projection and dependency rules remain explicit in
the family specifications. Numerical formulas, profile accuracy contracts and
caller preconditions belong to those specifications, separately from kernel
scheduling and resource behavior.

See the [numeric implementation reference](../../kernel-architecture/Numeric-Operations.md),
[accelerated specification](op_specs/NUM_accelerated_contract.md#current-implementation),
[mathematical implementation](math-implementation.md),
[LUT3D baking](op_specs/CRV-09_bake_lut3d.md#fixed-geometry-tasks) and
[static mapping contract](../../kernel-architecture/Dependency-Data.md#static-mapping-contract).

Numerical and workflow checks for bundled operations are created for the
change and removed after use; independent oracles and package contracts are
retained. Specification acceptance and implementation completion remain
separate states.
