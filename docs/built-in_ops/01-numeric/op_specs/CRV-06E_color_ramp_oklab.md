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
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-06E: color_ramp_oklab

The maintainer requires separate CIELAB, CIELCh(ab), OKLab and OKLCh ramp
implementations. This specification covers OKLab only, rather than an ambiguous
Lab mode. Use explicit stops and a model-described color array under the
[ramp family](CRV-06_color_ramp.md). This primitive's clarification is complete;
its ColorArray representation and registry implementations remain Proposed.

## Confirmed interpolation and white

Linearly interpolate the L, a, b components directly and return OKLab in the
fixed D65 reference white. Custom reference-white substitution is rejected. Do not
automatically convert the result to RGB or adapt between reference whites;
those are separate color-management operations. No RGB transfer/gamut parameter
is used to redefine the OKLab coordinate values.

All three OKLab components may take any finite values. L's customary [0,1]
reference range is documented but not a clipping/validity limit. a/b retain
their signed values. Signed/HDR coordinate extensions and out-of-RGB-gamut colors
are not rejected by this ramp; conversion/gamut policy belongs to other operators.

This version uses exactly L,a,b with no alpha. The maintainer selected the
same three-component-only scope for CIELAB, CIELCh(ab) and OKLCh, with coverage
handled separately. No four-channel perceptual-space association is inferred.

## Inherited interface and exact arithmetic

Inherit [RGB ramp](CRV-06A_color_ramp_rgb.md) for the explicit input/stops/colors
port order, static color-description matching, K=1..65536, finite strictly
increasing stops, clamp/reject default clamp, singleton behavior, independently
mixed Float32/Float64 inputs and output dtype defaulting to colors dtype.
Replace RGB/RGBA with colors[K,3] in L,a,b order and output input.shape+[3].
Input rank is 1..7 and all input/color/output logical counts are <=2^40.
The output is a described OKLab color array with unchanged reference white,
not a generic untagged array or inferred RGB Image. No ICC or transfer execution
is part of this primitive.

Required static String parameters are color_description, dtype and out_of_domain.
The description identifies OKLab, the fixed D65 white identity, L/a/b
channel units and alpha=none. No RGB primaries or RGB transfer override is accepted.
Direct nodes supply all parameters; constructors write dtype and clamp defaults.
An attached colors description must match the explicit description, including white.

For each selected adjacent pair, w=(input-stops[j])/(stops[j+1]-stops[j])
exactly, and component c returns RN_dtype((1-w)*colors[j,c]+w*colors[j+1,c]).
Treat inputs as exact binary rationals and round only once at the destination.
All three CPU versions are bitwise identical. No OKLab-to-XYZ, RGB conversion,
white adaptation or approximate perceptual model enters this arithmetic.
Exact hit/clamp/K=1 paths correctly convert the selected row directly.
Direct/identical-color paths preserve zero signs; other exact mixed zeros are +0,
and nonzero exact underflow preserves sign. Final overflow fails the full color.

## Demand, mapping and resources

The observation domain is input.shape, excluding the appended three channels.
Any channel request computes/validates and returns the complete color at that
position. Control support is all stops and requested input positions. Color
Data/Validation is exactly the selected complete one/two rows, with no remote
color scan. Both rows remain required on a constant-color shortcut. Empty Q
reads no payload; compile/preflight still validates descriptors and static edges.
Existing typed and upstream support closures remain explicit and retain identity.

Stop changes invalidate all dependent observations; position changes affect its
color; a changed selected row invalidates whole colors using it. Retain white/
model/parameter and input witnesses in cache identity. Return immutable packed
fragments at the correct global Region/storage origins, with the OKLab color
description. Legal immutable arbitrary strides, negative/zero strides, offsets
and unaligned input are supported. Owners remain valid beyond context lifetime.

For M requested colors, lookup work is O(K+M log K) plus exact three-component
interpolation and typed validation. Output payload is 3*M*b and optional stop
index is 8K bytes. Account descriptors, source owners/windows, complete-color
fragments, lookup metadata, arithmetic limbs and growth overlap under host
capacity/work/stage limits. Inherit RGB's cancellation intervals, cache-off,
ownership and failure-publication obligations without its transfer/alpha scratch.
No whole color-table or full logical output allocation is required.

## Errors, acceptance and implementation status

Malformed static parameters/white/model descriptions fail compile/preflight with
InvalidArgument/InvalidDomain; port shape/dtype or attached-description mismatch
uses TypeMismatch. Nonfinite demanded positions/colors, invalid stops and reject
domain failures use OperationFailed/InvalidDomain. Destination overflow uses
OperationFailed/ArithmeticOverflow at the complete color Atom. Host resource,
backend, upstream/typed, cancellation and stale errors retain their categories,
origin and scope. No partial successful Lab tuple is published.

Conceptual fixture: stops=[0,1], colors=[[0.25,0.125,-0.25],[0.75,-0.125,0.5]], input=[0.5]
-> values=[[0.5,0,0.125]], with the same explicit D65 OKLab description. Also use
L outside [0,1], large opposite signed a/b, white mismatch, all source/destination
dtype combinations, direct signed zeros and final narrowing overflow. Independent
exact rational interpolation and bit conversion are the numeric oracle.

The future public workflow binds input/stops/colors, supplies the complete
description/dtype/policy, and inspects named values and metadata through Compiler/
ExecutionContext. Delivery provides actual run commands and outputs. Verify
full-color request expansion, unselected invalid colors remaining unread,
global-stop failures, exact dirty witnesses, strides, low budgets, cancellation,
cache-off and descriptor/data lifetime after context teardown. No new runtime,
color conversion compatibility or platform result is claimed by this specification.

- [Color-array dependency](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Operator specification template](../../00-foundation/spec-template.md).

The [model author](https://bottosson.github.io/posts/oklab/) defines OKLab with
D65 and its own coordinate scale. This ramp interpolates supplied coordinates;
it does not adopt the reference conversion code as a bitwise conversion oracle.
