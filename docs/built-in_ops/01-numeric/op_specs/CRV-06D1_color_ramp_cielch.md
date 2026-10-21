---
spec_schema_version: 1
id: CRV-06D1
parent_id: CRV-06D
function: color_ramp_cielch
operation_family: curve.color_ramp_cielch
proposed_operation_keys:
  - curve.color_ramp_cielch_strict
  - curve.color_ramp_cielch_accelerated_apple_silicon
  - curve.color_ramp_cielch_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06D1: color_ramp_cielch

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

## Interface and purpose

This independent CIELCh(ab) ramp uses floating radian source hue.
The ordered dynamic inputs are input, stops, colors; output is named values.
colors is Float32/Float64[K,3], ordered l=L*/100, C*, h.
input and stops independently accept Float32/Float64. stops is [K] with
1<=K<=65536; input has rank 1..7, and values has shape sample_shape(input)+[3].
All logical element limits, matching K and finite-value rules come from the
[complete CIELCh contract](CRV-06D_color_ramp_cielch.md).

Static String parameters are color_description, dtype, out_of_domain
and output_hue_unit. Constructors default dtype to colors dtype,
out_of_domain to clamp, output_hue_unit to radian.
Both radian and pi_multiple output units remain available. color_description
explicitly identifies the input representation, CIELCh(ab), white (default D50)
and no alpha. The output parameter selects the floating hue unit; the Result carries no description.

## Inherited executable contract

All original-hue interpolation, C=0 hue retention, correct final
rounding and unit-conversion rules are normative from CRV-06D.
Strict is correctly rounded; accelerated finite arithmetic follows the shared
FP32-scaled final-result bound. No GPU variant is specified.
Exact hit/clamp directly converts the original hue; no source hue is discarded
because chroma is zero. A mismatched platform is not silently redirected.

Inherit the family's [fixed execution and caller preconditions](CRV-06_color_ramp.md#fixed-execution-and-caller-preconditions), including complete-color output tiles, conservative table dependencies, fixed scratch, cancellation and retained output lifetime. Selected hue and rational-denominator preconditions remain active at zero chroma or saturation. Unfinished certified arithmetic returns ResourceExhausted.

## Workflow and acceptance

Bind the ordered ports above, stops=[0,1], matching color_description, and
explicit static parameters. colors=[[0.25,2,0],[0.75,4,0]], input=[0.5] gives [[0.5,3,+0]].
The example uses the default output unit; values carries only tensor structure and samples.

Use the shared independent arithmetic oracle, per-profile/dtype/unit fixtures,
multi-turn and large-angle cases, full versus sparse complete-color requests, unused remote rows, strides, low-budget failures, cancellation and
owner-lifetime acceptance. Include source/destination precision mixing and finite huge hues.
The implementation section links the constructor and shared arithmetic contract.

## Maintained implementation and validation

The public helper `color_ramp_cielch_node` is declared in
[`color_ramps.hpp`](../../../../plugins/ops/include/photospider/ops/numeric/color_ramps.hpp).
`color_ramps.cpp` prepares immutable model/unit/profile state and executes the
fixed CPU tile kernel. Original hue values use exact rational interpolation;
Cross-unit conversion in either direction multiplies or divides by certified pi,
with a 4096-bit precision ceiling. Same-unit RationalPi expressions cancel pi
symbolically. Validate the Result path and numerical profile independently for each change. See the [family contract](CRV-06_color_ramp.md#maintained-implementation-and-validation)
for the shared behavior and acceptance requirements.
