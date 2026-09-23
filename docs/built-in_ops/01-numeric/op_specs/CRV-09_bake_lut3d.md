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
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-09: bake_lut3d workflow template

## Revised lightness coordinate and implementation boundary

The [2026-09-23 shared scale revision](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
requires native CIELAB/CIELCh l=L*/100, including ramp stops' color values,
LUT input axes and output table coordinates. Finite values outside 0..1 remain
legal. Opponent/chroma scales and arithmetic formulas are unchanged. Runtime
ColorArray v1 still encodes the old implicit L* units: public metadata, fixtures
and consumers need explicit migration before this revised target is implemented.
Historical implementation/test evidence below does not establish that migration;
no silent old/new unit alias or sample-magnitude inference is permitted.

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

Dynamic axis is Float64[3,3], with static shape=[N0,N1,N2]. Inherit CRV-07's
2..256 extents, independent ascending/descending axes, exact reconstructed
Float64 coordinates and complete axis validation. Evaluate the source at those
grid colors. Export table[N0,N1,N2,3] and axis with matching descriptions for
direct application by CRV-07.

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
Float64 validation_points[M,3]. The quality label is Measured, meaning evaluation
at the listed finite points completed; passed separately records acceptance.
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
fail; these are not successful completed reports with passed=false. Axis remains
independent. Report is a fixed-size summary: passed, Measured quality, grid shape,
method/tolerances, validation count/failed count, per-component maximum absolute
error and its point, plus the first failed point and source/LUT values. Never
store all per-point results. Process cell centers in logical row-major order,
then extra points in array order; this defines the first failure independently
of scheduling. Failed count counts points with any failing component.

Optional extra points have 1<=M<=1048576. Omit the input for no extra points;
do not use a zero-length Value. Repeated coordinates are allowed and count
separately, as do centers which round to repeated coordinates.

All validation points must pass for the current bound snapshot before any table
observation is published. An exceeded threshold fails the table request with
diagnostic point coordinates, component, source reference, LUT value and error.
The supplied grid shape is fixed; no automatic refinement occurs. Partial table
requests still require the global quality gate. No failed table is returned as a
successful result with only a warning report.

axis is independently observable: axis-only requests validate and return the
grid axis without source execution or error measurement. A nonempty table request
triggers source evaluation of all grid vertices and all validation points plus
the global acceptance gate, irrespective of its requested table subset. The final gate exports only requested table storage, while generated Whole
Values and source intermediates retain their full owners.

## Authoring and evaluation contract

Authoring binds the source input/output endpoints and exposes dynamic axis,
optional validation_points, and the graph's remaining declared shared inputs.
Required static parameters are shape (three integer extents), interpolation
(trilinear/tetrahedral), atol/rtol (Float64), table_dtype (Float32/Float64,
default source output dtype), input/output color descriptions and the explicit
source_pointwise assertion. Descriptions obey CRV-07's same-model requirements.
The assertion is caller responsibility, not a general compiler independence proof.
The source boundary takes one color per logical position with a final three-axis
component dimension; all execution batching must respect the asserted semantics.

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
fits the current <=16-field structural vocabulary.

| Field | Dtype | Rows / record shape | Meaning |
| --- | --- | --- | --- |
| passed | UInt8 | 1 / scalar | 1 iff every validation point passes |
| axis | Float64 | 3 / [3] | Validated dynamic axis used for this bake |
| validation_count | Int64 | 1 / scalar | All centers plus extra points, including repeats |
| failed_count | Int64 | 1 / scalar | Number of points failing one or more components |
| max_abs_error | Float64 | 1 / [3] | Per-component maximum, rounded upward; overflow +Inf |
| max_error_point | Float64 | 3 / [3] | Actual coordinate of each component's maximum |
| max_error_index | Int64 | 1 / [3] | First validation ordinal attaining each exact maximum |
| first_failure_index | Int64 | 1 / scalar | First failed ordinal, or -1 on pass |
| first_failure_input | Float64 | 1 / [3] | Actual query, or all +0 on pass |
| first_failure_reference | Float64 | 1 / [3] | Source color, or all +0 on pass |
| first_failure_lut | Float64 | 1 / [3] | Applied table color, or all +0 on pass |

Compare errors and tolerances using exact arithmetic before report rounding.
Choose maxima by exact absolute error, breaking ties with the earliest ordinal.
Report +Inf is a diagnostic bound for an unrepresentable error, not a nonfinite
source color or a failure of report validity. A zero maximum is +0. The fixed
field payload is 289 bytes; metadata and all backing capacities are additionally
budgeted. Report observations cannot publish an incomplete prefix while later
validation may alter counts/maxima. A passed report corresponds to the same
owned bake state/binding provenance as its gated table, not merely equal shapes.

## Demand, dirty propagation, ownership and resources

axis-only demand has all-axis Data/Validation support and no source/extra-point
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
checks, plus declared source work. The maintained generated Value nodes use complete dense Whole outputs: grid
3*V*8, validation points 3*P*8, and converted color arrays at their dtype widths.
Source work and outputs additionally follow each source operator contract.
Sampled-table Result backing stores all 3*V*b bytes; pack/unpack I/O remains
bounded by windows. All shared owners, scratch, staging and repeated work count
against host limits. No unbudgeted full source
batch or global table cache is allowed. The output subset does not waive global
work. A small reported managed payload is not a claim about whole-process RSS.

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
LutApproximationToleranceExceeded and first-failure details. This is a diagnostic
tag, not a new FailureReason enum. axis is unaffected by quality failure. table
success requires both the global source/model validation and passed report state.

Conceptual identity workflow on a 2x2x2 [0,1]^3 RGB grid passes with atol=rtol=0
under either interpolation method. For source (r,g,b)->(r*r,g,b), the same grid
and center [0.5,0.5,0.5] give source [0.25,0.5,0.5] versus LUT [0.5,0.5,0.5].
With atol=0.1,rtol=0, report passed=0,failed_count=1,max_abs_error[0]=0.25,
first_failure_index=0; table fails and axis-only still succeeds. Adding the same
center as an extra point increments validation_count and failed_count separately.

Output-dtype scope fixture: let the source explicitly cast Float64 colors to
Float32, with one grid axis [1,1+2^-23] and other axes [0,1]. At the cell center,
the first component is 1+2^-24 before source rounding. Both source and internal
Float32 LUT output round it to 1, so a zero-tolerance measurement can pass.
Applying that table later with Float64 output instead returns 1+2^-24 there;
that different output-dtype configuration is not covered by the passed report.

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
[`photospider/numeric/lut3d_baking.hpp`](../../../../include/photospider/numeric/lut3d_baking.hpp).
Its `Lut3dSourceBuilder` is authoring-only and is invoked twice while staging the
ordinary source graph: it appends nodes and returns outputs. The helper does not
retain the builder or its captures. The resulting graph executes the source
transform, whose pointwise independence is asserted by the caller. Grid and
validation evaluations share one immutable binding snapshot, including shared
inputs and optional `ResourceBindings`; no nested execution or file save is added.

`BakedLut3d` returns connectable table, axis and report references without
computing payloads during authoring. At execution, the report retains its owned
sampled-table Result association, which the gate checks before publishing table
fragments. The table is gated by the measured report; axis is independent. The registered
`curve.bake_lut3d.report` v1 CompleteBundle schema is read through
`read_lut3d_bake_report`, and its recipe/source identity and binding provenance
remain attached to the result. A failed tolerance report can be read as
`passed=false`, while the dependent table fails; source, resource, cancellation
and budget failures remain execution failures. There is no opaque compose key or
chain object.

All 15 formal `curve.bake_lut3d_{axis,grid,points,points_extra,color}` profile
keys execute Whole with complete inputs, complete dense outputs and numeric Run
failure scope. Any participating input edit invalidates the complete recorded
Value demand. Axis remains independent from source/extra data. The four
unsuffixed keys `curve.pack_lut3d`, `curve.measure_lut3d`, `curve.unpack_lut3d`
and `curve.gate_lut3d` retain ResultProtocol2 and compact source/descriptor
relations. Their structured schemas, report ordering, quality gate and ObjectId
association are unchanged. They are not legacy numeric aliases or Value Whole
callbacks.

Pack uses authorized rectangular collect into owned packed storage, at most
64 KiB and bounded by the Result page limit. Complete inner rows and planes are
combined when they fit; smaller limits split the innermost row in logical order.
Measure collects three authorized windows of up to 64 colors, at most 4608 bytes
combined, with 289 additional report bytes when preparing the final fields.
The immutable schema is decoded once and only model/tolerance POD fields are
retained. Finite/model checks and exact errors keep their order; one-row and
three-row immutable relations are shared across report fields. Grid and color
Value workspace is fixed plus three reconstructed axis indexes; all owners and
allocator/metadata overhead remain managed. A small requested table does not
reduce generated Whole storage or global measurement work.

Native Clang 21 strict/Apple each pass nine manual groups and 480 independent
Fraction workflow cases. Tests cover all five geometry kinds, arbitrary layouts,
active cancellation, workspace/output limits, row/plane boundaries through 64 KiB, 72-byte Result
windows, Float32/64 negative/unaligned packing, report/table association and
lifetime. Focused numeric/compiler/color/resource/Result execution and global
Result tests pass. No new x86 or installed-package run is claimed. See the
[workflow](../../../../examples/numeric_workflow/README.md#measured-three-dimensional-lut-baking)
and [math notes](../math-implementation.md#crv-09-measured-lut3d-baking)
for commands, performance and validation scope.

- [3D LUT application](CRV-07_apply_lut3d.md).
- [1D baking templates](CRV-04_bake_lut1d.md).
- [Curve category](../curves.md).
