---
spec_schema_version: 1
id: CRV-09
function: bake_lut3d
proposed_template_names:
  - curve.bake_lut3d
category: 01-numeric
kind: composite_workflow
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_subset
clarification_status: complete
---

# CRV-09: bake_lut3d workflow template

## Revised lightness coordinate and implementation boundary

The [2026-09-23 shared scale revision](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
requires native CIELAB/CIELCh l=L*/100, including ramp stops' color values,
LUT input axes and output table coordinates. Finite values outside 0..1 remain
legal. Opponent/chroma scales and arithmetic formulas are unchanged. The current runtime ColorArray v1 encodes L* in its implicit 0..100 unit;
this target scale applies only after public metadata, fixtures and consumers
adopt it explicitly. No implicit unit alias or inference from sample magnitude
is permitted.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM execution baseline](NUM_common_contract.md) for specification/
registration status, CPU target identity, floating environment, diagnostic provenance,
host observation isolation, resource accounting and public/performance acceptance.
In particular, rounding is nearest/ties-to-even with gradual underflow and the
caller floating environment is preserved. This is limited inheritance: the ports,
output kinds/facets, observation units, mathematical rounding boundaries and
explicit numerical/error rules in this specification take precedence. It does not
turn a composite template or structured Result into a generic NUM Value primitive.

Proposed authoring template name: curve.bake_lut3d. No operation key named
compose or opaque chain object is registered by this specification.

This scope bakes a pointwise, spatially independent, same-model color-transform
workflow into a three-dimensional LUT. Reuse CRV-07 table/axis/color descriptions.
The supported same-model spaces include RGB, XYZ, CIELAB, OKLab, CIELCh(ab),
OKLCh, HSL and YCbCr, matching CRV-07. XYZ uses relative coordinates with white
Y=1 and an explicit reference white. CRV-04 covers one-dimensional baking. Composition uses ordinary workflow edges;
no new opaque compose primitive or color-transform-chain object is introduced.

The caller designates a color input port and a same-model color output port.
Other inputs may be shared dynamic parameters. For fixed shared parameters the
transform must act independently on each color, with no dependence on position,
batch shape or other sampled colors. Spatial filters and reductions across the
sampling batch do not satisfy this promise. Ordinary graph edges supply the
composition. The caller may explicitly declare source pointwise independence
and assumes responsibility for that assertion; a compiler proof is not mandatory.
Structural type/model checks remain required. Shared parameters may originate
from independent upstream computation. Measured validation cannot establish that
an asserted custom source is actually position/batch independent.

The dynamic axis and optional validation points are each a Result with one tensor
member under an arbitrary schema id/version and key. Shapes come from the full
`sample_shape()`, including batch axes. Axis is Float64[3,3], with static
shape=[N0,N1,N2]. Inherit CRV-07's
2..256 extents, independent ascending/descending axes, exact reconstructed
Float64 coordinates and complete axis validation. Evaluate the source at those
grid colors. The exported table is a `photospider.tensor` Result with `samples`
of shape [N0,N1,N2,3], selected table dtype and ColorArray v1 output description;
its internal owned table uses `curve.bake_lut3d.table` v2 with tensor member
`colors`. Axis is an immutable Float64 Result tensor independent of source
execution, and its coordinates follow the input description.

The designated source color input is Float64 to receive grid coordinates without
implicit narrowing. A Float32-only internal operator must be preceded by explicit
cast within the source graph; that rounding is part of the baked transformation.
Source output may be Float32/Float64. Table defaults to source output dtype or
uses an explicitly chosen floating dtype conversion. Conversion correctly rounds
each source component once; finite/model legality is required after conversion.
Source graph operation keys/precision are retained as supplied, not implicitly
replaced by strict variants. The baked reference is that supplied graph's result.

Required static interpolation is trilinear or tetrahedral. Required atol/rtol
are nonnegative finite values. For each output component compare the applied
LUT value against the source workflow reference using
abs(lut-reference)<=atol+rtol*abs(reference), with exact comparison arithmetic
to avoid overflow changing the verdict. This is the discretization/representation
error of the baked table under the chosen method, independent of the correct
rounding guarantee of CRV-07 interpolation.

Validation covers every grid-cell center and optional caller-supplied dynamic
Float64 Result tensor `validation_points` with `sample_shape()` [M,3]. The quality label is Measured, meaning evaluation
at the listed finite points completed.
It is not a CertifiedBound over the continuous domain, whether passed is true
or false.
A certified whole-domain variant is outside this initial scope. Each cell center
coordinate is the exact average of its two actual Float64 grid coordinates,
correctly rounded to Float64. If that rounds to a vertex, record the actual
coordinate and do not claim coverage of an unrepresentable interior point.
Extra validation points must be finite, satisfy the input model and lie within
the actual numeric range of every axis; no clamp or normalization is applied.
An independent structured report output is required in addition to table and
axis. report-only observation triggers the global baking/validation work. An
error-threshold violation yields a successful report with passed=false while
table remains gated and fails. Source computation errors, invalid inputs,
cancellation or insufficient resources make dependent report/table evaluation
fail. Axis remains
independent. Never
store all per-point results. Process cell centers in logical row-major order,
then extra points in array order; this defines the first failure independently
of scheduling. Failed count counts points with any failing component.

Optional extra points have 1<=M<=1048576. Omit the input for no extra points;
do not use a zero-length Result tensor. Repeated coordinates are allowed and
count separately, as do centers which round to repeated coordinates.

All validation points must pass for the current bound snapshot before any table
observation is published. An exceeded threshold fails the table request with
diagnostic point coordinates, component, source reference, LUT value and error.
The supplied grid shape is fixed; no automatic refinement occurs. Partial table
requests still require the global quality gate. No failed table is returned as a
successful result with only a warning report.

Axis is independently observable: axis-only requests validate and return the
grid axis without source execution or error measurement. A nonempty table request
triggers source evaluation of all grid vertices and all validation points plus
the global acceptance gate, irrespective of its requested table subset. The final
gate publishes the requested color ROI with its original global coordinates;
generated Whole Result outputs and source intermediates retain their full owners.

## Authoring and evaluation contract

Authoring binds the source input/output endpoints and exposes dynamic axis,
optional validation_points, and the graph's remaining declared shared inputs.
Required static parameters are shape (three integer extents), interpolation
(trilinear/tetrahedral), atol/rtol (Float64), table_dtype (Float32/Float64,
default source output dtype), input/output color descriptions and the explicit
source_pointwise assertion. Descriptions obey CRV-07's same-model requirements.
The assertion is caller responsibility, not a general compiler independence proof.
The source builder receives Result tensor references under any schema id/key;
each source result must contain one tensor with a `sample_shape()` matching the
generated input and Float32/64 dtype. Any attached ColorArray facet must match
the required color description; a generic tensor may omit that facet. The source
boundary takes one color per logical position with a final extent-3 component
axis; all execution batching must respect the asserted semantics.

All source calls, grid generation, table conversion and validation share one
immutable execution binding snapshot, including shared parameters. No mutable
file, changing profile or new input snapshot may be silently read between grid
and validation evaluations. A cached report cannot validate a new parameter
snapshot or a different table dtype/method. A source which violates its pointwise
assertion has no promised sampling-order-independent bake semantics.

Validate all grid colors against the input model, all source output colors against
the output model, and converted table colors against that output description.
Nonfinite or model-invalid values fail computation. Validation compares the
actual source output (exactly promoted to Float64 if necessary) to CRV-07's
correctly rounded applied LUT output at table_dtype, with no hidden higher-precision
replacement of the source. Internal CRV-07 application explicitly supplies
dtype=table_dtype and out_of_domain=reject; it does not use input-dtype defaults.
The chosen interpolation and applied output dtype are part of the quality
contract; using a different method or applied output dtype is not covered by
this measurement.

No automatic file save or one-time freeze occurs. Changes to shared dynamic
parameters or axis produce new execution-dependent results and new validation.
The maintained authoring API supplies explicit source sampling, global validation
and Result publication through ordinary graph nodes. It remains a Proposed
workflow contract rather than a new opaque runtime composition primitive.

## Structured report schema

Use registered SchemaTemplate id curve.bake_lut3d.report, version 1,
PublishPolicy::CompleteBundle, observation domain [1]. All fields have fixed
row counts. Metadata records quality=Measured, shape, interpolation, atol, rtol,
table/source dtypes, input/output descriptions and source recipe identity.
Dynamic axis is a data field, not compile-time metadata. The registered schema
fits the current <=16-field structural vocabulary. Use `ExecutionContext::execute()`
to observe these fields; `execute_fragments()` requests tensor footprints and
does not treat the report's fixed fields as a generic [1] tensor.

Compare errors and tolerances using exact arithmetic before report rounding.
Choose maxima by exact absolute error, breaking ties with the earliest ordinal.
Report +Inf is a diagnostic bound for an unrepresentable error, not a nonfinite
source color or a failure of report validity. A zero maximum is +0. The fixed
field payload is 289 bytes; metadata and all backing capacities are additionally
budgeted. Report observations cannot publish an incomplete prefix while later
validation may alter counts/maxima. A passed report corresponds to the same
owned bake state/binding provenance as its gated table, not merely equal shapes.

## Demand, dirty propagation, ownership and resources

Axis-only demand has all-axis Data/Validation support and no source/extra-point
payload support. report or nonempty table demand has global support: axis,
all generated grid inputs, the complete source outputs for grid/validation
points, every extra point and all shared inputs actually required by source
execution. Retain source typed/control/validation support, including exceptions
to local reads required by the source contract. Calling a source pointwise does
not erase its metadata/resource dependencies.

Any participating axis, extra point or shared source dependency change invalidates
the global report and every gated table observation. A source graph/descriptor
change requires the corresponding authoring/compilation update. table and report
may share physical bake work, but separate and joint observations have the same
meaning. Table backing is immutable, with requested fragments mapped to global
[i0,i1,i2,c] origins. axis is owned immutable Float64 data. ResultRef/descriptor
and table/read-window owners outlive their execution context, with exact source
and schema identity preserved. No temporary pointer stands in for shared state.

Let V=N0*N1*N2 and P=(N0-1)*(N1-1)*(N2-1)+M. A table/report observation evaluates
V grid colors and P reference colors, then P applied-LUT queries and exact error
checks, plus declared source work. The 15 geometry keys return Result tensors
with complete Whole outputs: grid inputs use 3*V*8 bytes, validation points use
3*P*8 bytes, and converted color arrays use their selected dtype widths. Source
work and outputs additionally follow each source operator contract. The sampled
table Result owns 3*V*b bytes; pack, unpack and gate retain legal authorized
views, with packed materialization only for an ROI whose physical mapping is
unavailable. They do not unconditionally collect and recopy the complete table.
All shared owners,
scratch, staging and repeated work count against host limits. No unbudgeted full
source batch or global table cache is allowed. A small requested ROI does not
waive global measurement work. Managed payload accounting is not a claim about
whole-process RSS.

Account report/schema, grids, source compiled state, callback/recipe captures,
snapshot and read-window ancestry, staging/validation buffers and scratch overlap.
Poll cancellation during grid/center enumeration, source invocation, extra-point
scans and error reduction, and inherit the participating operators' bounded
polling obligations. Exhausted budget/cancellation cannot yield a partial report
as though acceptance had completed. Release all temporary state on terminal paths;
result-owned metadata and data remain until their final owner releases them.

## Failure model and acceptance

Malformed authoring bindings, unsupported model/shape and invalid tolerances fail
compile/preflight with InvalidArgument/InvalidDomain; incompatible ports/source
boundaries/descriptions use TypeMismatch. Invalid dynamic axes/validation points,
model-invalid or nonfinite source results fail OperationFailed/InvalidDomain.
Finite source output narrowing to nonfinite table values fails
OperationFailed/ArithmeticOverflow. Source/upstream errors retain their origin.
Method-unavailable, ResourceExhausted, cancellation and stale-binding failures
retain established host categories and affect only dependent outputs.

A completed measurement with failed_count>0 leaves report successful but causes
table observation to fail OperationFailed/InvalidDomain with diagnostic tag
LutApproximationToleranceExceeded and first-failure details.

 For source (r,g,b)->(r*r,g,b), the same grid
and center [0.5,0.5,0.5] give source [0.25,0.5,0.5] versus LUT [0.5,0.5,0.5]. Adding the same
center as an extra point increments validation_count and failed_count separately.

Output-dtype scope fixture: let the source explicitly cast Float64 colors to
Float32, with one grid axis [1,1+2^-23] and other axes [0,1]. At the cell center,
the first component is 1+2^-24 before source rounding. Both source and internal
Float32 LUT output round it to 1, so a zero-tolerance measurement can pass.

Validate exact report maxima/tie order, both methods/dtypes, mixed source precision,
explicit source casts, hue multi-turn values, XYZ white scale, shared parameter
changes, rounded centers, extra-point bounds/count, and caller-asserted source
limitations. Test report-only/table-only/axis-only and joint requests, all-grid
source failures outside requested table fragments, cache-off, cancellation,
budget failure, strides and result/data lifetime. Verify finite sampled tests
never become a CertifiedBound claim. The maintained public template construction,
invocation commands and independent result checks are linked below; this specification remains Proposed.

## Maintained implementation and validation

The public `bake_lut3d` authoring entry point is declared in
[`photospider/ops/numeric/lut3d_baking.hpp`](../../../../plugins/ops/include/photospider/ops/numeric/lut3d_baking.hpp).
Its authoring-only `Lut3dSourceBuilder` runs for generated grid and validation
shapes, appends ordinary nodes and returns Result outputs. The helper protects
existing input declarations, Result schemas, nodes and exports; it retains no
builder or capture. Both expansions are analyzed by Compiler and execute under
one frozen binding snapshot, including shared inputs and optional
`ResourceBindings`. Pointwise independence remains the caller's assertion.

`BakedLut3d` returns connectable Result references for table, axis and report
without computing payloads during authoring. The exported table is an immutable tensor
`photospider.tensor` Result with member `samples`, shape [N0,N1,N2,3], selected
table dtype, ColorArray v1 facet and `atomic_trailing_axes=1`. The internal owned
table is a `curve.bake_lut3d.table` v2 Result with one `colors` tensor of the
same shape and dtype, the ColorArray facet, and measured metadata. The report is
the registered `curve.bake_lut3d.report` v1 Result with 11 fixed fields and a
289-byte field payload, read through
`read_lut3d_bake_report`. Recipe/source identity and binding provenance remain
attached to the Results. Measure's Result association records observed input
ObjectIds in port order. Its first entries are the whole axis, grid and owned
table Results; later entries can include multiple owners from validation-point,
reference and applied-result batches. Association entry 2 is the owned-table
ObjectId, which the gate matches to its actual table input. A completed
failed-tolerance report remains readable as
`passed=false`, while the dependent table fails with the measured-quality
diagnostic. Source, resource, cancellation and budget failures remain execution
failures. There is no opaque compose key or chain object.

All 15 formal `curve.bake_lut3d_{axis,grid,points,points_extra,color}` profile
keys execute Whole Result programs with role-13 input validation, complete output
coverage and numeric Run failure scope. Any participating input edit invalidates
the complete recorded demand. Axis remains independent from source/extra data.
Static `prepare_static` validates geometry parameters and Result metadata, then
stores immutable geometry and bake POD state for reuse during execution; runtime
callbacks do not reparse static parameters. The four unsuffixed keys
`curve.pack_lut3d`, `curve.measure_lut3d`, `curve.unpack_lut3d` and
`curve.gate_lut3d` are Result-only operations. Their schemas, report ordering,
quality gate and ObjectId association are explicit Result contracts.

Pack, Unpack and Gate first request authorized Result tensor views. Legal
common-owner mappings remain views. A physical `ViewUnavailable` causes
transactional materialization of only the requested region, preserving
dependency support and sample bits while charging Root Payload. Pack requests
the complete table, so its fallback may materialize the full table. The harness
checks Float32/Float64 across all five geometries with reversed and unaligned
layouts; a legal read window points into the original backing address with zero
Root Payload. Pack uses two Result polls on this path, without the former
Value-copy poll.

Measure reads authorized Result windows and appends the eleven report fields
only after complete measurement. Gate reads and validates all report fields
before its table Need; the largest field needs a 72-byte window. It then requests
the table with role 13 and typed validation, checks association entry 2, and
publishes the requested complete-color region at its global coordinates. An
association mismatch need not precede table-pixel validation. Empty tensor
coverage skips payload validation. The v2 owned-table validator preserves
status code, reason, origin and scope, including WorkLimit.

Axis grid storage, prepared report/geometry state, source program state, table
output, read-window ancestry, exact arithmetic workspace, metadata and allocator
overhead remain managed. Table, axis and report Results can be held independently;
a retained table read window may keep its backing alive after output handles are
released. The harness observes zero live Root capacity after the final window is
released, and verifies that external axis backing expires after context and
fixture retirement. A small requested table ROI does not reduce source
evaluation, Whole outputs or global measurement work.

 The tests cover
both table dtypes, all five geometries, layouts, demand and source-error order,
Empty requests, cache association with current inputs, preparation reuse and
rebinding, report gating, view and owner lifetime. A huge-grid case verifies
CapacityLimit at its geometry node; it does not show successful maximum-shape
execution. The cache fixture uses a 64 MiB Result cache, 1,048,576 proof units
for `maximum_dependency_cache_metadata`, and 128 Mi dependency-cache work units.
It observes 14 cache hits in this run, while its assertion requires only a
positive count; the observed count is not a contract. The metadata limit counts
proof units rather than bytes.

Its bake
assertions are independent fixture coverage, not all cases from the baking3d
harness. Exact focused CTest
and installed-consumer commands are maintained in the
[workflow section](../../../../examples/numeric_workflow/README.md#measured-three-dimensional-lut-baking).
No current x86, native-GPU or maximum-shape-success validation is claimed.

- [3D LUT application](CRV-07_apply_lut3d.md).
- [1D baking templates](CRV-04_bake_lut1d.md).
- [Curve category](../curves.md).
