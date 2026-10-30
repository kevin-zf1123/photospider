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
implementation_status: implemented
clarification_status: complete
---

# CRV-09: bake_lut3d workflow template

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Inherit the [NUM execution baseline](NUM_common_contract.md) for specification/registration status, CPU target identity and floating environment. Rounding is nearest/ties-to-even with gradual underflow. This is limited inheritance: the ports, output structures, observation units, mathematical rounding boundaries and explicit numerical/error rules in this specification take precedence. It does not turn a composite template or structured Result into a generic NUM Tensor primitive.

The authoring template name is curve.bake_lut3d; no operation key named compose or opaque chain object is registered. The geometry profile keys (`axis`, `grid`, `points`, `points_extra`, `color`) and the four Result operations `curve.pack_lut3d`, `curve.measure_lut3d`, `curve.unpack_lut3d` and `curve.gate_lut3d` are ordinary registered operations; the authoring helper builds ordinary source, sampling, application, measurement and gate nodes.

## Native lightness and explicit description

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md) uses native CIELAB/CIELCh `l=L*/100` for ramp colors, LUT input axes, output table coordinates and validation points. Finite values outside 0..1 remain legal. Opponent/chroma scales, hue winding and arithmetic formulas are unchanged. Color configuration uses canonical TensorDescription v4/v5 tuple parameters; explicit L* units are rejected without sample-magnitude inference.

## Scope

This scope bakes a pointwise, spatially independent, same-model color-transform workflow into a three-dimensional LUT. Reuse CRV-07 table/axis/color descriptions. The supported same-model spaces include RGB, XYZ, CIELAB, OKLab, CIELCh(ab), OKLCh, HSL and YCbCr, matching CRV-07. XYZ uses relative coordinates with white Y=1 and an explicit reference white. CRV-04 covers one-dimensional baking. Composition uses ordinary workflow edges; no new opaque compose primitive or color-transform-chain object is introduced.

The caller designates a color input port and a same-model color output port. Other inputs may be shared dynamic parameters. For fixed shared parameters the transform must act independently on each color, with no dependence on position, batch shape or other sampled colors. Spatial filters and reductions across the sampling batch do not satisfy this promise. Ordinary graph edges supply the composition. The caller may explicitly declare source pointwise independence and assumes responsibility for that assertion; a compiler proof is not mandatory. Structural type/model checks remain required. Shared parameters may originate from independent upstream computation. Measured validation cannot establish that an asserted custom source is actually position/batch independent.

The dynamic axis and optional validation points are each a Result with one tensor member under an arbitrary schema id/version and key. Shapes come from the full `sample_shape()`, including batch axes. Axis is Float64[3,3], with static shape=[N0,N1,N2]. Inherit CRV-07's 2..256 extents, independent ascending/descending axes, exact reconstructed Float64 coordinates. Finite endpoints, consistent representable step and strict RN64 reconstructed ordering are caller preconditions for geometry and CRV-07 application. Evaluate the source at those grid colors. The exported table is a `photospider.tensor` Result with `samples` of shape [N0,N1,N2,3], selected table dtype and one atomic trailing component axis; the intermediate owned table uses `curve.bake_lut3d.table` v4 with tensor member `colors`. Axis is an immutable Float64 Result tensor independent of source execution, and its coordinates follow the input description.

The designated source color input is Float64 to receive grid coordinates without implicit narrowing. A Float32-only internal operator must be preceded by explicit cast within the source graph; that rounding is part of the baked transformation. Source output may be Float32/Float64. Table defaults to source output dtype or uses an explicitly chosen floating dtype conversion. Conversion correctly rounds each source component once; finite/model legality is required after conversion. Source graph operation keys/precision are retained as supplied, not implicitly replaced by strict variants. The baked reference is that supplied graph's result.

Required static interpolation is trilinear or tetrahedral. Required atol/rtol are nonnegative finite values. For each output component compare the applied LUT value against the source workflow reference using abs(lut-reference)<=atol+rtol*abs(reference), with exact comparison arithmetic to avoid overflow changing the verdict. This is the discretization/representation error of the baked table under the chosen method, independent of the correct rounding guarantee of CRV-07 interpolation.

Validation covers every grid-cell center and optional caller-supplied dynamic Float64 Result tensor `validation_points` with `sample_shape()` [M,3]. The quality label is Measured, meaning evaluation at the listed finite points completed. It is not a CertifiedBound over the continuous domain, whether passed is true or false. A certified whole-domain variant is outside this initial scope. Each cell center coordinate is the exact average of its two actual Float64 grid coordinates, correctly rounded to Float64. If that rounds to a vertex, record the actual coordinate and do not claim coverage of an unrepresentable interior point. Extra validation points must be finite, satisfy the input model and lie within the actual numeric range of every axis; no clamp or normalization is applied. An independent structured report output is required in addition to table and axis. report-only observation triggers the global baking/validation work. An error-threshold violation yields a successful report with passed=false while table remains gated and fails. Actual source computation errors, cancellation or insufficient resources make dependent report/table evaluation fail. Invalid numerical inputs violate caller premises without a diagnostic guarantee. Axis remains independent. Never store all per-point results. Process cell centers in logical row-major order, then extra points in array order; this defines the first failure independently of scheduling. Failed count counts points with any failing component.

Optional extra points have 1<=M<=1048576. Omit the input for no extra points; do not use a zero-length Result tensor. Repeated coordinates are allowed and count separately, as do centers which round to repeated coordinates.

All validation points must pass for the current bound snapshot before any table observation is published. An exceeded threshold fails the table request with `LutApproximationToleranceExceeded` and `first_failure_index`. The explicit report contains the measured failure input, reference, LUT value and maximum errors. The supplied grid shape is fixed; no automatic refinement occurs. Partial table requests still require the global quality gate. No failed table is returned as a successful result with only a warning report.

Axis is independently observable: it is copied from the input axis without source execution or error measurement. Axis numerical consistency remains a caller precondition. A nonempty table request requires source evaluation of all grid vertices and all validation points plus the global acceptance gate, irrespective of its requested table subset. The gate publishes the requested color ROI with its original global coordinates.

## Interface

Authoring binds the source input/output endpoints and exposes dynamic axis, optional validation_points, and the graph's remaining declared shared inputs. Required static parameters are shape (three integer extents), interpolation (trilinear/tetrahedral), atol/rtol (Float64), table_dtype (Float32/Float64, default source output dtype), input/output color descriptions and the explicit source_pointwise assertion. Descriptions are TensorDescription logical tuples encoded by `tensor_description_parameter`; they obey CRV-07's same-model, three-component, no-alpha and floating-hue requirements. The assertion is caller responsibility, not a general compiler independence proof. The source builder receives Result tensor references under any schema id/key; each source result must contain one tensor with a `sample_shape()` matching the generated input and Float32/64 dtype. The caller supplies input/output color descriptions as static bake parameters; no facet is read from source Results. The source boundary takes one color per logical position with a final extent-3 component axis; all execution batching must respect the asserted semantics.

All source calls, grid generation, table conversion and validation share one immutable execution binding snapshot, including shared parameters. The report and measured table belong to the same workflow pairing and snapshot. A source which violates its pointwise assertion has no promised sampling-order-independent bake semantics.

Finite model-valid grid colors, extra points and contributing source colors are caller preconditions for fixed geometry. Converted colors must remain finite and model-valid in their selected dtype. The geometry kernels perform no sample validation or narrowing-overflow diagnostic pass. Validation compares the actual source output (exactly promoted to Float64 if necessary) to CRV-07's correctly rounded applied LUT output at table_dtype, with no hidden higher-precision replacement of the source. Internal CRV-07 application explicitly supplies dtype=table_dtype and out_of_domain=reject; it does not use input-dtype defaults. The chosen interpolation and applied output dtype are part of the quality contract; using a different method or applied output dtype is not covered by this measurement.

No automatic file save or one-time freeze occurs. Changes to shared dynamic parameters or axis produce new execution-dependent results and new validation. The template is a workflow contract built from ordinary graph nodes, not an opaque runtime composition primitive.

## Native lightness and tolerance

The native lightness coordinate is `l=L*/100` in the source grid, extra points, table values, reference values and recorded failure coordinates. The scalar `atol` applies to each of the three stored output components, while `rtol` uses the existing relative-error formula. `atol` is stated in native units for all three components; it is not rescaled per channel.

### Structured report schema

The SDK defines SchemaTemplate id `curve.bake_lut3d.report` at version 4 with thirteen static Tensor members. The measurement publishes the report after evaluating the complete validation set, and the SDK report reader requires complete report coverage. A partial public projection can expose selected members, while measurement still writes the complete report before any member becomes available.

| Tensor member | Dtype and shape |
| --- | --- |
| passed | UInt8 [1] |
| axis | Float64 [3,3] |
| validation_count, failed_count | Int64 [1] each |
| max_abs_error | Float64 [1,3] |
| max_error_point | Float64 [3,3] |
| max_error_index | Int64 [1,3] |
| first_failure_index | Int64 [1] |
| first_failure_input, first_failure_reference, first_failure_lut | Float64 [1,3] each |
| configuration_id | UInt8 [1,64], lowercase hexadecimal SHA-256 |
| measured_table_id | UInt8 [1,8], little-endian ObjectId |

`configuration_id` is the lowercase hexadecimal SHA-256 of the shape, interpolation, tolerances, source/table dtypes, extra-point count, explicit color descriptions and source recipe, under the `photospider.bake-configuration.v2` hash domain. Both exact canonical TensorDescription byte strings enter the digest. Static helpers receive `Lut3dBakeDescription`; the runtime reader receives a previously prepared `Lut3dBakeReportLayout`.

`measured_table_id` is an ordinary `UInt8[1,8]` payload encoding the ObjectId of the complete owned table read during measurement, in little-endian order. The producer records the logical ObjectId of its owned-table input. ObjectId is process-local identity, not a content digest: two tables with identical bytes but different ObjectIds are not interchangeable for this quality report.

Prepare `Lut3dBakeReportLayout` from the static report schema before execution. Call `read_lut3d_bake_report(result, layout)` only on a completed Result covering all thirteen members, under the prepared schema and payload premises. The reader accesses completed data without starting production. Axis values are ordinary payload rather than dynamic schema extents.

Errors and tolerances are compared using exact arithmetic before report rounding. Choose maxima by exact absolute error, breaking ties with the earliest ordinal. Report +Inf is a diagnostic bound for an unrepresentable error, not a nonfinite source color or a failure of report validity. A zero maximum is +0. A report observation cannot publish an incomplete prefix while later validation may alter counts/maxima. A passed report corresponds to the same owned bake state as its gated table, not merely equal shapes.

### Geometry sampling

Grid coordinates use endpoint-weighted RN64 reconstruction as in CRV-07. Each cell center averages the two actual neighboring reconstructed Float64 knots and rounds that exact average to Float64. This preserves descending axes, endpoint raw bits and rounded-to-vertex centers. Extra validation points follow the centers. Color conversion copies same-dtype bits or performs one direct RN conversion. Finite consistent axis data, model-valid colors, in-range extras and representable converted outputs are caller preconditions.

### Report reader and gate

The report layout is prepared once from static metadata and reused for completed reports:

```cpp
// Before execution: baked is numeric::BakedLut3d.
auto schema = ps::lut3d_bake_schema(baked.description);
if (!schema.ok()) return schema.status();
auto layout = ps::prepare_lut3d_bake_report_layout(schema.value());
if (!layout.ok()) return layout.status();
// After execution: result is the complete report ResultRef.
auto measured = ps::read_lut3d_bake_report(result, layout.value());
if (!measured.ok()) return measured.status();
const bool passed = measured.value().passed;
```

The authoring entry point `bake_lut3d` takes a `Lut3dSourceBuilder` that runs for the generated grid and validation shapes, appends ordinary nodes and returns Result outputs; the helper protects existing input declarations, Result schemas, nodes and exports. `BakedLut3d` returns connectable Result references for table, axis and report, plus an owning `description` configuration value. No payload is computed during authoring. The exported table is `photospider.tensor` v1/member `samples`, shape [N0,N1,N2,3], selected dtype and atomic_trailing_axes=1; the intermediate table is `curve.bake_lut3d.table` v4/member `colors` with the same shape and dtype. Neither contains a domain description or quality metadata.

Gate consumes the report's `passed`: false returns `OperationFailed`/`InvalidDomain` with `LutApproximationToleranceExceeded` and the recorded first_failure_index. It does not compare configuration or ObjectId, scan the report or recompute error samples. Report-only execution succeeds for a legitimate failed measurement. Numerical legality, report consistency and the configuration/ObjectId association are caller premises.

## Behavior

The axis output depends only on the input axis. The report or a nonempty table has global dependency: the axis, all generated grid inputs, the complete source outputs for grid/validation points, every extra point and all shared inputs required by source execution. A source's pointwise assertion does not erase its own dependencies. A new Run using changed axis, extra-point or shared source inputs computes a new report and gated table; table and report may share bake work, but separate and joint observations have the same meaning. Requested table fragments map to global [i0,i1,i2,c] origins. A small requested ROI does not waive global measurement work, and the measurement cannot publish a partial report as though acceptance had completed.

Let V=N0*N1*N2 and P=(N0-1)*(N1-1)*(N2-1)+M. A table/report observation evaluates V grid colors and P reference colors, then P applied-LUT queries and exact error checks.

Malformed authoring bindings, unsupported model/shape and malformed parameter syntax/types fail with InvalidArgument/InvalidDomain; finite nonnegative tolerances are unchecked premises. Incompatible ports/source boundaries/descriptions use TypeMismatch. Dynamic axis consistency, validation-point bounds/model legality, finite representable color conversion and well-formed report values are unchecked premises with no promised runtime diagnostic. Source/upstream errors retain their origin. A completed measurement with failed_count>0 leaves the report successful but causes table observation to fail OperationFailed/InvalidDomain with diagnostic tag LutApproximationToleranceExceeded and first_failure_index.

## Acceptance

For source (r,g,b)->(r*r,g,b), the same grid and center [0.5,0.5,0.5] give source [0.25,0.5,0.5] versus LUT [0.5,0.5,0.5]. Adding the same center as an extra point increments validation_count and failed_count separately.

Output-dtype scope fixture: let the source explicitly cast Float64 colors to Float32, with one grid axis [1,1+2^-23] and other axes [0,1]. At the cell center, the first component is 1+2^-24 before source rounding. Both source and internal Float32 LUT output round it to 1, so a zero-tolerance measurement can pass.

Validate exact report maxima/tie order, both methods/dtypes, mixed source precision, explicit source casts, hue multi-turn values, XYZ white scale, shared parameter changes, rounded centers, extra-point bounds/count, and caller-asserted source limitations. Report-only, table-only, axis-only and joint requests are covered, including all-grid source failures outside requested table fragments. Finite sampled tests never become a CertifiedBound claim. Expected geometry values (RN coordinates, rounded centers, dtype conversion) come from independent exact evaluation; a Float32 source case evaluates Float64 source arithmetic followed by a distinct Float32 cast, preserving that graph's rounding boundaries and raw zero signs.

## References

- [3D LUT application](CRV-07_apply_lut3d.md).
- [1D baking templates](CRV-04_bake_lut1d.md).
- [Curve category](../curves.md).
