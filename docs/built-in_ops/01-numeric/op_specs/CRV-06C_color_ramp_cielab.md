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

Dynamic inputs inherit the [family Result tensor-port contract](CRV-06_color_ramp.md#result-tensor-ports): each is a Result with exactly one tensor member, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

## Native lightness and explicit description

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
uses native CIELAB/CIELCh `l=L*/100` for ramp colors, LUT input axes, output
table coordinates and validation points. Finite values outside 0..1 remain
legal. Opponent/chroma scales, hue winding and arithmetic formulas are unchanged.
The implementation uses canonical TensorDescription v4/v5 tuple parameters.
The old ColorArray types and decoder are removed; old payloads and explicit
L* units are rejected without sample-magnitude inference.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

The maintainer requires separate CIELAB, CIELCh(ab), OKLab and OKLCh ramp
implementations. This specification covers CIELAB only, rather than an ambiguous
Lab mode. Use explicit stops and a model-described color array under the
[ramp family](CRV-06_color_ramp.md). This primitive's clarification is complete;
its normalized native coordinates and TensorDescription configuration are
implemented; the specification remains Proposed.

## Confirmed interpolation and white

Linearly interpolate the normalized l=L*/100, a*, b* components directly and return CIELAB in the
same explicitly declared reference white. The authoring default is D50. Do not
automatically convert the result to RGB or adapt between reference whites;
those are separate color-management operations. No RGB transfer/gamut parameter
is used to redefine the CIELAB coordinate values.

All three CIELAB components may take any finite values. l's nominal [0,1]
reference range is documented but not a clipping/validity limit. a*/b* retain
their signed values. Signed/HDR coordinate extensions and out-of-RGB-gamut colors
are not rejected by this ramp; conversion/gamut policy belongs to other operators.

This version uses exactly l,a*,b* with no alpha. The maintainer selected the
same three-component-only scope for CIELCh(ab), OKLab and OKLCh, with coverage
handled separately. No four-channel perceptual-space association is inferred.

## Inherited interface and exact arithmetic

Inherit [RGB ramp](CRV-06A_color_ramp_rgb.md) for the explicit input/stops/colors
port order, static color-description matching, K=1..65536, finite strictly
increasing stops, clamp/reject default clamp, singleton behavior, independently
mixed Float32/Float64 inputs and output dtype defaulting to colors dtype.
Replace RGB/RGBA with colors[K,3] in l,a*,b* order and output sample_shape(input)+[3].
Input rank is 1..7 and all input/color/output logical counts are <=2^40.
The output coordinates use the same explicit reference white; the Result carries no color description. No ICC or transfer execution
is part of this primitive.

Required static String parameters are color_description, dtype and out_of_domain.
The description identifies CIELAB, an explicit white xy (default D50), l/a*/b*
channel units and alpha=none. No RGB primaries or RGB transfer override is accepted.
Direct nodes supply all parameters; constructors write dtype and clamp defaults.
Results carry no description; the static parameters define the coordinate interpretation.

For each selected adjacent pair, w=(input-stops[j])/(stops[j+1]-stops[j])
exactly, and component c returns RN_dtype((1-w)*colors[j,c]+w*colors[j+1,c]).
Treat inputs as exact binary rationals and round only once at the destination.
Strict is correctly rounded; accelerated finite arithmetic follows the shared
FP32-scaled final-result bound. No Lab-to-XYZ, RGB conversion,
white adaptation or approximate perceptual model enters this arithmetic.
Exact hit/clamp/K=1 paths correctly convert the selected row directly.
Direct/identical-color paths preserve zero signs; other exact mixed zeros are +0,
and nonzero exact underflow preserves sign. Finite final output is a caller precondition.

## Demand, mapping and resources

All profiles use the [fixed execution contract](CRV-06_color_ramp.md#fixed-execution-and-caller-preconditions). Each selected complete-color output tile has one CPU task. Its reads cover the corresponding query region and all stops/colors/p/q tiles, each closed on its own source grid. Runtime selection uses one exact-hit/clamp/singleton row or two enclosing rows. Generic unselected colors and denominators are numerically unused; upstream failures still propagate through declared tile dependencies.

The `values` Result uses `photospider.tensor` v1/member `samples`, descriptor shape `sample_shape(input)+[C]`, and `atomic_trailing_axes=1`. It carries no color description or input batch-axis partition. The kernel writes only selected dense output tiles, borrowing affine source views and owning fixed arithmetic scratch. Empty emits no task or sample read. Successful call publication follows required task completion in retirement order, and output storage survives context teardown.

Finite ordered stops, finite queries, selected complete-color model/association validity, selected rational q>0, reject-domain membership and representable final output are unchecked caller preconditions. Lookup costs O(P log K) for P selected query points, plus exact/certified arithmetic; the kernel searches source stops directly without an O(K) copy. Static schemas, parameters, resources and kernel signatures remain checked. Cancellation and capacity failures stop execution without approximate substitution.

## Errors, acceptance and implementation status

Malformed static parameters or descriptions fail preparation with InvalidArgument; incompatible port schemas, shapes or dtypes use TypeMismatch. Numerical conditions are caller preconditions, including finite destination results. Operational resource, backend, cancellation, stale-plan and upstream failures preserve their status; a failed required task prevents successful call publication.

Fixture: stops=[0,1], colors=[[0.25,10,-20],[0.75,-10,40]], input=[0.5]
-> values=[[0.5,0,10]], with the same explicit D50 CIELAB description. Also use
l outside [0,1], large opposite signed a*/b*, white mismatch, all source/destination
dtype combinations, direct signed zeros and finite final narrowing. Independent
exact rational interpolation and bit conversion are the numeric oracle.

A temporary public workflow should bind input/stops/colors, supply the complete
description/dtype/policy, and inspect named values and metadata through Compiler/
ExecutionContext. Use independent exact-rational or certified-pi/transfer references on valid inputs. Check complete-color tile coverage, mixed source grids and dtypes, unused generic rows, arbitrary source strides, concurrent Runs, cancellation, bounded scratch, allocation-free invocation and retained output lifetime. Numerical precondition violations are not runtime-error acceptance cases. This coordinate
interpolation does not perform color-model conversion.

- [Color-array dependency](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Operator specification template](../../00-foundation/spec-template.md).
