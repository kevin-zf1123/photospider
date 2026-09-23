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
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-06C: color_ramp_cielab

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
Replace RGB/RGBA with colors[K,3] in l,a*,b* order and output input.shape+[3].
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

All formal strict and accelerated keys use Whole execution. Any nonempty
request collects complete input, stops and color arrays (and both rational-hue
integer arrays when present), with complete upstream and typed validation.
The callback validates every stop, then every position, before color arithmetic.
Each position still uses exactly one hit/clamp/singleton row or two enclosing
rows mathematically. Unused generic color rows are not subjected to new numeric
domain checks; invalid typed data or upstream failures anywhere still fail.
Empty requests perform static preflight but read no sample payload.

The output is one immutable dense Value of shape input.shape+[C]. The final
channel axis retains complete-color closure, ColorArray identity and owned ICC
resources where applicable. Public fragments expose the requested complete
colors while retaining the full output owner. Arbitrary immutable input strides,
offsets and unaligned storage are supported. Owners survive context teardown.
Any input edit invalidates the complete recorded output demand. Cache identity
retains descriptors, parameters, typed validation and resource identities.
Numeric errors have Run scope; no successful color subset survives a failed
callback. Upstream, resource and cancellation errors retain their categories.

For N=product(input.shape), lookup work is O(K+N log K), plus actual exact or
certified arithmetic. Full output payload is N*C*sizeof(dtype), even for a small
requested region. Account complete collected inputs, fixed admitted arithmetic
workspace, a ResourceVector stop index with 8K element bytes plus allocator and
metadata overhead, and retained descriptors/resources. No per-output dependency
records or point-state array is retained. Work/capacity limits and cancellation
apply during scans, lookup, arithmetic and before publication; incomplete
certification fails ResourceExhausted. No reduced-precision fallback is added.

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

## Maintained implementation and validation

Public [`color_ramp_cielab_node`](../../../../include/photospider/numeric/color_ramps.hpp)
constructs this primitive; [`color_ramps.cpp`](../../../../plugins/ops/01-numeric/color_ramps.cpp)
implements its Whole complete-output execution.

All profiles use exact rational component interpolation and return strict
bits, with one destination rounding. Complete-color metadata and complete-input typed
validation remain attached to the result.

See the [family implementation](CRV-06_color_ramp.md#maintained-implementation-and-validation),
[mathematical machinery](../math-implementation.md#crv-06-colorarray-and-color-ramps)
and [public workflow commands](../../../../examples/numeric_workflow/README.md#color-ramps)
for resource limits and actual validation.
