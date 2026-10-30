---
spec_schema_version: 1
id: CRV-06C
parent_id: CRV-06
function: color_ramp_cielab
operation_family: curve.color_ramp_cielab
proposed_operation_keys:
  - curve.color_ramp_cielab_strict
  - curve.color_ramp_cielab_accelerated_apple_silicon
  - curve.color_ramp_cielab_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06C: color_ramp_cielab

Dynamic inputs follow the [family Result tensor-port contract](CRV-06_color_ramp.md#interface): each is a Result with exactly one tensor member, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Native lightness and explicit description

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md) uses native CIELAB/CIELCh `l=L*/100` for ramp colors, LUT input axes, output table coordinates and validation points. Finite values outside 0..1 remain legal. Opponent/chroma scales, hue winding and arithmetic formulas are unchanged. Color configuration uses canonical TensorDescription v4/v5 tuple parameters; explicit L* units are rejected without sample-magnitude inference.

CIELAB, CIELCh(ab), OKLab and OKLCh have separate ramp specifications. This specification covers CIELAB only, rather than an ambiguous Lab mode, using explicit stops and a model-described color array under the [ramp family](CRV-06_color_ramp.md).

## Interface

The ports, K=1..65536, finite strictly increasing stops, clamp/reject (default clamp), singleton behavior, independently mixed Float32/Float64 inputs and output dtype defaulting to colors dtype follow the [RGB ramp](CRV-06A_color_ramp_rgb.md), with colors[K,3] in l,a*,b* order and output sample_shape(input)+[3]. Input rank is 1..7 and all input/color/output logical counts are <=2^40. The output coordinates use the same explicit reference white; the Result carries no color description. No ICC or transfer execution is part of this operation.

Required static String parameters are color_description, dtype and out_of_domain. The description identifies CIELAB, an explicit white xy (default D50), l/a*/b* channel units and alpha=none. No RGB primaries or RGB transfer override is accepted. Direct nodes supply all parameters; constructors write dtype and clamp defaults. The static parameters define the coordinate interpretation.

The `values` Result uses `photospider.tensor` v1/member `samples`, descriptor shape `sample_shape(input)+[3]`, and `atomic_trailing_axes=1`; it carries no color description or input batch-axis partition.

Malformed static parameters or descriptions fail with InvalidArgument; incompatible port schemas, shapes or dtypes use TypeMismatch.

## Mathematics

The normalized l=L*/100, a*, b* components are interpolated linearly and directly, and CIELAB is returned in the same explicitly declared reference white (authoring default D50). The result is not converted to RGB or adapted between reference whites; no RGB transfer/gamut parameter redefines the CIELAB coordinate values. This version uses exactly l,a*,b* with no alpha; no four-channel perceptual-space association is inferred.

All three components may take any finite values. l's nominal [0,1] reference range is documented but not a clipping/validity limit. a*/b* retain their signed values. Signed/HDR coordinate extensions and out-of-RGB-gamut colors are not rejected by this ramp; conversion/gamut policy belongs to other operators.

For each selected adjacent pair, w=(input-stops[j])/(stops[j+1]-stops[j]) exactly, and component c returns RN_dtype((1-w)*colors[j,c]+w*colors[j+1,c]). Inputs are exact binary rationals rounded only once at the destination. Strict is correctly rounded; accelerated finite arithmetic follows the shared FP32-scaled final-result bound. No Lab-to-XYZ, RGB conversion, white adaptation or approximate perceptual model enters this arithmetic. Exact hit/clamp/K=1 paths correctly convert the selected row directly. Direct/identical-color paths preserve zero signs; other exact mixed zeros are +0, and nonzero exact underflow preserves sign.

## Behavior

Each output color depends on its query value and the selected one or two stop rows; all stops and colors are the dependency support. Generic unselected colors are numerically unused. Region and tile results equal the corresponding slice of the whole result. Finite ordered stops, finite queries, reject-domain membership and finite final output are unchecked caller preconditions.

## Acceptance

Fixture: stops=[0,1], colors=[[0.25,10,-20],[0.75,-10,40]], input=[0.5] -> values=[[0.5,0,10]], with the same explicit D50 CIELAB description. Also use l outside [0,1], large opposite signed a*/b*, white mismatch, all source/destination dtype combinations, direct signed zeros and finite final narrowing. Exact rational interpolation and bit conversion give the expected values. Mixed source grids, dtypes and unused generic rows are covered. This coordinate interpolation does not perform color-model conversion.

## References

- [Color-array dependency](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Operator specification template](../../00-foundation/spec-template.md).
