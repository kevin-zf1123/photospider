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
implementation_status: implemented_subset
clarification_status: complete
---

# CRV-06C: color_ramp_cielab

Dynamic inputs inherit the [family Result tensor-port contract](CRV-06_color_ramp.md#result-tensor-ports): each is a Result with exactly one tensor member and no fields, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

## Revised lightness coordinate and implementation boundary

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
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

The maintainer requires separate CIELAB, CIELCh(ab), OKLab and OKLCh ramp
implementations. This specification covers CIELAB only, rather than an ambiguous
Lab mode. Use explicit stops and a model-described color array under the
[ramp family](CRV-06_color_ramp.md). This primitive's clarification is complete;
its numerical interpolation is available, while the revised coordinate metadata
remains pending as noted above; the specification remains Proposed.

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
The output is a described CIELAB color array with unchanged reference white,
not a generic untagged array or inferred RGB Image. No ICC or transfer execution
is part of this primitive.

Required static String parameters are color_description, dtype and out_of_domain.
The description identifies CIELAB, an explicit white xy (default D50), l/a*/b*
channel units and alpha=none. No RGB primaries or RGB transfer override is accepted.
Direct nodes supply all parameters; constructors write dtype and clamp defaults.
An attached colors description must match the explicit description, including white.

For each selected adjacent pair, w=(input-stops[j])/(stops[j+1]-stops[j])
exactly, and component c returns RN_dtype((1-w)*colors[j,c]+w*colors[j+1,c]).
Treat inputs as exact binary rationals and round only once at the destination.
Strict is correctly rounded; accelerated finite arithmetic follows the shared
FP32-scaled final-result bound. No Lab-to-XYZ, RGB conversion,
white adaptation or approximate perceptual model enters this arithmetic.
Exact hit/clamp/K=1 paths correctly convert the selected row directly.
Direct/identical-color paths preserve zero signs; other exact mixed zeros are +0,
and nonzero exact underflow preserves sign. Final overflow fails the full color.

## Demand, mapping and resources

All formal strict and accelerated keys use Whole Result programs. A nonempty
Run declares Data, Validation and Descriptor needs (role 13) for every input, so
typed and upstream validation covers complete tensor members. The executor reads
authorized windows directly; the callback does not collect or copy complete input
arrays. It validates all stops, then every input position, before color arithmetic.
Each position uses one exact hit/clamp/singleton row or two enclosing rows for its
mathematics. Generic color rows outside all evaluated stencils remain numerically
unused, while their typed/upstream validation still applies. Empty reads no sample
payload after static preparation.

The `values` port publishes an immutable `photospider.tensor` v1 Result with
`samples` shape `sample_shape(input)+[C]`, the ColorArray v1 facet and
`atomic_trailing_axes=1`. Whole writes publish the full output transactionally;
the Result retains full certified coverage and global coordinates. Fragments expose
requested regions while retaining the full output owner. Arbitrary immutable source
strides, offsets and unaligned storage are supported. Result association records
source ObjectIds, and selected ColorArray resources remain owned by the output.
Input edits invalidate complete recorded output demand. Numeric errors have Run
scope; failed callbacks publish no color subset. Upstream, resource and cancellation
errors retain their categories.

Lookup work remains O(K+N log K), plus exact or certified arithmetic. Full output
payload is N*C*sizeof(dtype), even for a small request. Account source owners/windows,
fixed arithmetic workspace, the Root-owned ResourceVector stop index (8K element
bytes plus allocator/metadata overhead), output and retained descriptors/resources.
No per-output dependency records or point-state array is retained. Work/capacity and
cancellation checks apply during scans, lookup, arithmetic and before publication;
incomplete certification fails ResourceExhausted. No reduced-precision fallback is added.

## Errors, acceptance and implementation status

Malformed static parameters/white/model descriptions fail compile/preflight with
InvalidArgument/InvalidDomain; port shape/dtype or attached-description mismatch
uses TypeMismatch. Nonfinite demanded positions/colors, invalid stops and reject
domain failures use OperationFailed/InvalidDomain. Destination overflow uses
OperationFailed/ArithmeticOverflow at Run scope. Host resource,
backend, upstream/typed, cancellation and stale errors retain their categories,
origin and scope. No partial successful Lab tuple is published.

Fixture: stops=[0,1], colors=[[0.25,10,-20],[0.75,-10,40]], input=[0.5]
-> values=[[0.5,0,10]], with the same explicit D50 CIELAB description. Also use
l outside [0,1], large opposite signed a*/b*, white mismatch, all source/destination
dtype combinations, direct signed zeros and final narrowing overflow. Independent
exact rational interpolation and bit conversion are the numeric oracle.

The public workflow binds input/stops/colors, supplies the complete
description/dtype/policy, and inspects named values and metadata through Compiler/
ExecutionContext; run commands and checked results are linked below. Verify
full-color request expansion, mathematically unused generic colors and complete typed validation,
global-stop failures, whole-input dirty witnesses, strides, low budgets, cancellation,
cache-off and descriptor/data lifetime after context teardown. This coordinate
interpolation does not perform color-model conversion.

- [Color-array dependency](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Operator specification template](../../00-foundation/spec-template.md).
