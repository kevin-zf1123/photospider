---
spec_schema_version: 1
id: CRV-06E
parent_id: CRV-06
function: color_ramp_oklab
operation_family: curve.color_ramp_oklab
proposed_operation_keys:
  - curve.color_ramp_oklab_strict
  - curve.color_ramp_oklab_accelerated_apple_silicon
  - curve.color_ramp_oklab_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06E: color_ramp_oklab

Dynamic inputs follow the [family Result tensor-port contract](CRV-06_color_ramp.md#interface): each is a Result with exactly one tensor member, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

CIELAB, CIELCh(ab), OKLab and OKLCh have separate ramp specifications. This specification covers OKLab only, rather than an ambiguous Lab mode, using explicit stops and a model-described color array under the [ramp family](CRV-06_color_ramp.md).

## Interface

The ports, K=1..65536, finite strictly increasing stops, clamp/reject (default clamp), singleton behavior, independently mixed Float32/Float64 inputs and output dtype defaulting to colors dtype follow the [RGB ramp](CRV-06A_color_ramp_rgb.md), with colors[K,3] in L,a,b order and output sample_shape(input)+[3]. Input rank is 1..7 and all input/color/output logical counts are <=2^40. The output coordinates use the same fixed D65 reference white; the Result carries no color description. No ICC or transfer execution is part of this operation.

Required static String parameters are color_description, dtype and out_of_domain. The description identifies OKLab, the fixed D65 white identity, L/a/b native channel units and no alpha. `tensor_description_parameter` encodes the canonical logical tuple with `channel_axis=0`. No RGB primaries or RGB transfer override is accepted, and custom reference-white substitution is rejected. Direct nodes supply all parameters; constructors write dtype and clamp defaults. The static parameters define the coordinate interpretation.

The `values` Result uses `photospider.tensor` v1/member `samples`, descriptor shape `sample_shape(input)+[3]`, and `atomic_trailing_axes=1`; it carries no color description or input batch-axis partition. Malformed static parameters or descriptions fail with InvalidArgument; incompatible port schemas, shapes or dtypes use TypeMismatch.

## Mathematics

The L, a, b components are interpolated linearly and directly, and OKLab is returned in the fixed D65 reference white. The result is not converted to RGB or adapted between reference whites; no RGB transfer/gamut parameter redefines the OKLab coordinate values. This version uses exactly L,a,b with no alpha; no four-channel perceptual-space association is inferred.

All three components may take any finite values. L's customary [0,1] reference range is documented but not a clipping/validity limit. a/b retain their signed values. Signed/HDR coordinate extensions and out-of-RGB-gamut colors are not rejected by this ramp; conversion/gamut policy belongs to other operators.

For each selected adjacent pair, w=(input-stops[j])/(stops[j+1]-stops[j]) exactly, and component c returns RN_dtype((1-w)*colors[j,c]+w*colors[j+1,c]). Inputs are exact binary rationals rounded only once at the destination. Strict is correctly rounded; accelerated finite arithmetic follows the shared FP32-scaled final-result bound. No OKLab-to-XYZ, RGB conversion, white adaptation or approximate perceptual model enters this arithmetic. Exact hit/clamp/K=1 paths correctly convert the selected row directly. Direct/identical-color paths preserve zero signs; other exact mixed zeros are +0, and nonzero exact underflow preserves sign.

## Behavior

Each output color depends on its query value and the selected one or two stop rows; all stops and colors are the dependency support. Generic unselected colors are numerically unused. Region and tile results equal the corresponding slice of the whole result. Finite ordered stops, finite queries, reject-domain membership and finite final output are unchecked caller preconditions.

## Acceptance

Fixture: stops=[0,1], colors=[[0.25,0.125,-0.25],[0.75,-0.125,0.5]], input=[0.5] -> values=[[0.5,0,0.125]], with the same explicit D65 OKLab description. Also use L outside [0,1], large opposite signed a/b, white mismatch, all source/destination dtype combinations, direct signed zeros and finite final narrowing. Exact rational interpolation and bit conversion give the expected values. This coordinate interpolation does not perform color-model conversion.

## References

- [Color-array dependency](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Operator specification template](../../00-foundation/spec-template.md).
- The [model author](https://bottosson.github.io/posts/oklab/) defines OKLab with D65 and its own coordinate scale. This ramp interpolates supplied coordinates; it does not adopt the reference conversion code as a bitwise conversion oracle.
