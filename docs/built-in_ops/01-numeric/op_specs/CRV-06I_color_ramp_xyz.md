---
spec_schema_version: 1
id: CRV-06I
parent_id: CRV-06
function: color_ramp_xyz
operation_family: curve.color_ramp_xyz
proposed_operation_keys:
  - curve.color_ramp_xyz_strict
  - curve.color_ramp_xyz_accelerated_apple_silicon
  - curve.color_ramp_xyz_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06I: color_ramp_xyz

Dynamic inputs inherit the [family Result tensor-port contract](CRV-06_color_ramp.md#interface): each is a Result with exactly one tensor member, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Model and purpose

Interpolate supplied X,Y,Z components directly and return relative XYZ with the same explicit reference white. White normalization is Y=1, default white D65; D50 and custom white xy are supported. Absolute luminance and white-Y=100 data require explicit conversion. This primitive has exactly three channels, no alpha, and accepts all finite signed/HDR component values. It performs no transfer, RGB conversion, white adaptation, gamut clamp or nominal-range clipping.

XYZ is also included in CRV-07's same-model three-dimensional LUT and CRV-09 baking scope. It is useful for explicit color-management composition; it need not be presented as a default manual grading model. The [ICC XYZ profile library](https://registry.color.org/profile-library/xyz-profiles) documents XYZ encoding/interchange use; this ramp does not execute ICC profiles.

## Interface and mathematics

The dynamic port order input, stops, colors follows [CIELAB CRV-06C](CRV-06C_color_ramp_cielab.md), with colors as XYZ[K,3]. Output values has shape sample_shape(input)+[3], with XYZ interpretation supplied by the static parameters. Floating ports independently accept Float32/Float64, output defaults to colors dtype. Input rank is 1..7, K=1..65536, all extents positive and all logical element counts <=2^40. Stops are finite strictly increasing; clamp/reject defaults to clamp. Singleton and finite input rules are unchanged from CRV-06C.

Required static String parameters are color_description, dtype, out_of_domain. The description identifies XYZ, explicit reference-white xy, relative white-Y=1 scale and alpha=none. No RGB primaries/transfer parameter reinterprets XYZ data. Generic colors are interpreted by this explicit declaration. The output uses the same white and scale without attached metadata.

For each component, use the exact stop-derived w and return RN_dtype((1-w)*c0+w*c1), rounding the complete formula once. Strict is correctly rounded; accelerated follows the shared FP32-scaled bound. Direct hit/clamp/K=1 correctly converts the selected row. Direct/identical-row zero signs are preserved, genuine mixed exact zero is +0 and nonzero underflow retains sign. All mathematically selected source components and output components must be finite; no spurious intermediate overflow may reject a representable result.

## Behavior

Each output color depends on its query value and the selected one or two stop rows; all stops and colors are the dependency support. Generic unselected colors are numerically unused. Region and tile results equal the corresponding slice of the whole result.

The `values` Result uses `photospider.tensor` v1/member `samples`, descriptor shape `sample_shape(input)+[3]`, and `atomic_trailing_axes=1`. It carries no color description or input batch-axis partition.

Finite ordered stops, finite queries, reject-domain membership and finite final output are unchecked caller preconditions. Malformed static parameters or descriptions fail with InvalidArgument; incompatible port schemas, shapes or dtypes use TypeMismatch.

## Acceptance

Fixture: stops=[0,1], colors=[[0,0,0],[0.5,1,1.5]], input=[0.5] returns [[0.25,0.5,0.75]] with the selected relative-XYZ/white description. Use independent exact rational interpolation and destination rounding, negative values, HDR, extreme cancellation, mixed dtype, direct signed zeros, white/scale mismatches, full-color request expansion and remote invalid rows.

## References

- [Generic color-array contract](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Color ramp family](CRV-06_color_ramp.md).
