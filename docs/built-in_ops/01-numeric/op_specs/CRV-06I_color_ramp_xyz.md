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

Dynamic inputs inherit the [family Result tensor-port contract](CRV-06_color_ramp.md#result-tensor-ports): each is a Result with exactly one tensor member and no fields, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

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
sample_shape(input)+[3] and a complete XYZ description. Floating ports independently
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
RN_dtype((1-w)*c0+w*c1), rounding the complete formula once. Strict is correctly rounded; accelerated follows the shared FP32-scaled bound. Direct hit/clamp/K=1 correctly converts the
selected row. Direct/identical-row zero signs are preserved, genuine mixed exact
zero is +0 and nonzero underflow retains sign. All demanded and output components
must be finite; no spurious intermediate overflow may reject a representable result.

## Demand, resources and failures

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

Malformed descriptors, white/scale and statics fail preflight with
InvalidArgument/InvalidDomain; shape/dtype or description mismatch uses TypeMismatch.
Nonfinite demanded inputs/colors, invalid stops and rejected domain queries use
OperationFailed/InvalidDomain; final narrowing overflow uses
OperationFailed/ArithmeticOverflow. Resource, backend, cancellation, stale and
upstream errors retain their established identity. No partial XYZ tuple succeeds.

## Acceptance and current implementation boundary

Fixture: stops=[0,1], colors=[[0,0,0],[0.5,1,1.5]], input=[0.5]
returns [[0.25,0.5,0.75]] with the selected relative-XYZ/white description.
Use independent exact rational interpolation and destination rounding, negative
values, HDR, extreme cancellation, mixed dtype, direct signed zeros, white/scale
mismatches, full-color request expansion, remote invalid rows, whole-input dirty mapping,
strides, low budgets, cancellation, cache-off and owner lifetime acceptance.

Existing XYZ image/model conversion support does not implement this generic
color-ramp interface. The maintained public Compiler/ExecutionContext bindings, commands and independently
checked values/metadata are linked below; the document remains Proposed.

- [Generic color-array contract](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Color ramp family](CRV-06_color_ramp.md).
