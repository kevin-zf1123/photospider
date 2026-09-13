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
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-06I: color_ramp_xyz

## Model and purpose

Interpolate supplied X,Y,Z components directly and return relative XYZ with the
same explicit reference white. White normalization is Y=1, default white D65;
D50 and custom white xy are supported. Absolute luminance and white-Y=100 data
require explicit conversion. This primitive has exactly three channels, no alpha,
and accepts all finite signed/HDR component values. It performs no transfer,
RGB conversion, white adaptation, gamut clamp or nominal-range clipping.

XYZ is also included in CRV-07's same-model three-dimensional LUT and CRV-09
baking scope. It is useful for explicit color-management composition; it need
not be presented as a default manual grading model. The
[ICC XYZ profile library](https://registry.color.org/profile-library/xyz-profiles)
documents XYZ encoding/interchange use; this ramp does not execute ICC profiles.

## Full interface and numerical contract

Inherit [CIELAB CRV-06C](CRV-06C_color_ramp_cielab.md) for dynamic port order
input, stops, colors, replacing colors with XYZ[K,3]. Output values has shape
input.shape+[3] and a complete XYZ description. Floating ports independently
accept Float32/Float64, output defaults to colors dtype. Input rank is 1..7,
K=1..65536, all extents positive and all logical element counts <=2^40.
Stops are finite strictly increasing; clamp/reject defaults to clamp. Singleton
and finite input rules are unchanged from CRV-06C.

Required static String parameters are color_description, dtype, out_of_domain.
The description identifies XYZ, explicit reference-white xy, relative white-Y=1
scale and alpha=none. No RGB primaries/transfer parameter reinterprets XYZ data.
Generic colors may be interpreted by this explicit declaration; an attached
color description must match. Output retains white and scale exactly.

For each component, use the exact stop-derived w and return
RN_dtype((1-w)*c0+w*c1), rounding the complete formula once. All three CPU
versions are bitwise identical. Direct hit/clamp/K=1 correctly converts the
selected row. Direct/identical-row zero signs are preserved, genuine mixed exact
zero is +0 and nonzero underflow retains sign. All demanded and output components
must be finite; no spurious intermediate overflow may reject a representable result.

## Demand, resources and failures

Inherit CRV-06C's complete-color observation domain (input.shape), global stop
validation, requested position reads, exactly one/two complete selected color
rows, explicit typed/upstream closures, exact dirty witnesses and cache identity.
Any channel request observes the full XYZ tuple. Empty Q reads no dynamic data;
unselected invalid colors are not observed. Both rows remain dependencies for
an identical-color shortcut. Descriptor/white identity is retained in caching.

Immutable arbitrary strides, offsets and unaligned input are supported. Return
packed full-color fragments with correct global Region/storage origins and
owners surviving context teardown. Output payload is 3*M*sizeof(dtype) for M
requested colors; lookup work O(K+M log K) plus exact arithmetic and optional
8K-byte stop lookup. Budget all backing owners, windows, descriptor data,
exact limbs and scratch growth. Inherit bounded cancellation, capacity/work/stage,
cache-off and atomic failed-color publication from CRV-06C.

Malformed descriptors, white/scale and statics fail preflight with
InvalidArgument/InvalidDomain; shape/dtype or description mismatch uses TypeMismatch.
Nonfinite demanded inputs/colors, invalid stops and rejected domain queries use
OperationFailed/InvalidDomain; final narrowing overflow uses
OperationFailed/ArithmeticOverflow. Resource, backend, cancellation, stale and
upstream errors retain their established identity. No partial XYZ tuple succeeds.

## Acceptance and current implementation boundary

Conceptual fixture: stops=[0,1], colors=[[0,0,0],[0.5,1,1.5]], input=[0.5]
returns [[0.25,0.5,0.75]] with the selected relative-XYZ/white description.
Use independent exact rational interpolation and destination rounding, negative
values, HDR, extreme cancellation, mixed dtype, direct signed zeros, white/scale
mismatches, full-color request expansion, remote invalid rows, exact dirty mapping,
strides, low budgets, cancellation, cache-off and owner lifetime acceptance.

Existing XYZ image/model conversion support does not implement this generic
color-ramp interface. Delivery must provide actual public Compiler/ExecutionContext
bindings, run commands and independently checked values/metadata. This Proposed
document claims no current color_ramp_xyz registration or runtime test result.

- [Generic color-array contract](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Color ramp family](CRV-06_color_ramp.md).
