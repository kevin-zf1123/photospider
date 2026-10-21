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
scratch. The report records the actual owned-table ObjectId. Configuration and
table association are unchecked premises. Gate uses zero arithmetic scratch and
reads only passed and first_failure_index; it does not repeat measurement. The
SDK reader decodes using a statically prepared Lut3dBakeReportLayout. An ordered
retirement observer may retain a successful report when a later Gate fails.

Authoring templates expand ordinary workflow nodes with independent output
identities. Their output projection and dependency rules remain explicit in
the family specifications. Numerical formulas, profile accuracy contracts and
caller preconditions belong to those specifications, separately from kernel
scheduling and resource behavior.

The independent NUM/CRV color migration is complete. CRV-06/07/09 use existing
TensorDescription v4/v5 logical tuples, normalized Lab/LCh lightness and straight
RGB storage. Old ColorArray interfaces/decoding and RGB association options are
removed. Bake report/owned-table schema v4 and configuration hash domain v2 retire
old payload identities. The kernel remains frozen. See the
[migration implementation and validation record](NUM-CRV_color_migration.md).

See the [numeric implementation reference](../../kernel-architecture/Numeric-Operations.md),
[accelerated specification](op_specs/NUM_accelerated_contract.md#current-implementation),
[mathematical implementation](math-implementation.md),
[LUT3D baking](op_specs/CRV-09_bake_lut3d.md#fixed-geometry-tasks) and
[static mapping contract](../../kernel-architecture/Dependency-Data.md#static-mapping-contract).

Numerical and workflow checks for bundled operations are created for the
change and removed after use; independent oracles and package contracts are
retained. Specification acceptance and implementation completion remain
separate states.

## Unchecked revision and verification (2026-10-21)

Static schema/parameter structure and fixed invocation signatures remain checked.
Runtime counts, offsets, indices, associations, canonical payload encoding and
numerical/geometric conditions are unchecked producer/consumer premises,
including numerical domains supplied as static parameters. Slice/index/Bezier/q
address guards, numeric-domain admission and completed-report validation were
removed. Exact arithmetic, precision refinement, explicit clipping, cancellation
and real resource-capacity failures retain their operation semantics.

LUT report reading now requires a `Lut3dBakeReportLayout` prepared from the static
v4 schema. The former runtime description overload is removed. Configuration ID
and measured table ID are decoded as data. Gate trusts the measured `passed`
outcome and `first_failure_index`, with zero arithmetic scratch and no association
comparison or repeated error calculation. The report's thirteen-member v4 wire
schema remains unchanged.

The existing `build/ops-dev` target `photospider_ops` built successfully with its
SDK and private color engine dependencies on macOS arm64. Modified C/C++ sources
passed ClangFormat 21 and cpplint. A temporary public Compiler/prepare/submit/join
program passed 33 focused cross-family checks: legal slice/index/quantile/Bezier
and exact arithmetic; legacy curve/LUT behavior; static index/report schema
rejection; format clipping and IEEE bits; native tone/display and OCIO; successful
and failed LUT measurements, prepared report reading and explicit Gate outcomes;
report-window capacity/cancellation; quality bound calculation and transactional
diagnostic-counter capacity failure. External fixtures used the public DenseHost
COPY adapter; completed producer tiles used DirectTiles for subsequent bindings.
Temporary test sources and binaries were removed after execution. This evidence
does not establish a full numeric, performance or cross-platform conformance run.
