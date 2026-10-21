---
spec_schema_version: 1
id: CRV-06G2
parent_id: CRV-06G
function: color_ramp_hsl_pi
operation_family: curve.color_ramp_hsl_pi
proposed_operation_keys:
  - curve.color_ramp_hsl_pi_strict
  - curve.color_ramp_hsl_pi_accelerated_apple_silicon
  - curve.color_ramp_hsl_pi_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06G2: color_ramp_hsl_pi

Dynamic inputs inherit the [family Result tensor-port contract](CRV-06_color_ramp.md#result-tensor-ports): each is a Result with exactly one tensor member, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and purpose

This independent HSL ramp uses floating pi-multiple source hue.
The ordered dynamic inputs are input, stops, colors; output is named values.
colors is Float32/Float64[K,3], ordered H, S, L.
input and stops independently accept Float32/Float64. stops is [K] with
1<=K<=65536; input has rank 1..7, and values has shape sample_shape(input)+[3].
All logical element limits, matching K and finite-value rules come from the
[complete HSL contract](CRV-06G_color_ramp_hsl.md).

Static String parameters are color_description, dtype, out_of_domain
and output_hue_unit. Constructors default dtype to colors dtype,
out_of_domain to clamp, output_hue_unit to pi_multiple.
Both radian and pi_multiple output units remain available. color_description
explicitly identifies the input representation, HSL, underlying RGB primaries/white/transfer (default sRGB/D65/sRGB)
and no alpha. The output parameter selects the floating hue unit; the Result carries no description.

## Inherited executable contract

All original-hue interpolation, S=0 hue retention, correct final
rounding and unit-conversion rules are normative from CRV-06G.
Strict is correctly rounded; accelerated finite arithmetic follows the shared
FP32-scaled final-result bound. No GPU variant is specified.
Exact hit/clamp directly converts the original hue; no source hue is discarded
because saturation is zero. A mismatched platform is not silently redirected.

Inherit the family's [fixed execution and caller preconditions](CRV-06_color_ramp.md#fixed-execution-and-caller-preconditions), including complete-color output tiles, conservative table dependencies, fixed scratch, cancellation and retained output lifetime. Selected hue and rational-denominator preconditions remain active at zero chroma or saturation. Unfinished certified arithmetic returns ResourceExhausted.

## Workflow and acceptance

Bind the ordered ports above, stops=[0,1], matching color_description, and
explicit static parameters. colors=[[0,0.25,0.125],[4,0.75,0.875]], input=[0.5] gives [[2,0.5,0.5]].
The example uses the default output unit; values carries only tensor structure and samples.

Use the shared independent arithmetic oracle, per-profile/dtype/unit fixtures,
multi-turn and large-angle cases, full versus sparse complete-color requests, unused remote rows, strides, low-budget failures, cancellation and
owner-lifetime acceptance. Include source/destination precision mixing and finite huge hues.
The implementation section links the constructor and shared arithmetic contract.

## Maintained implementation and validation

The public helper `color_ramp_hsl_pi_node` is declared in
[`color_ramps.hpp`](../../../../plugins/ops/include/photospider/ops/numeric/color_ramps.hpp).
`color_ramps.cpp` prepares immutable model/unit/profile state and executes the
fixed CPU tile kernel. Original hue values use exact rational interpolation;
Cross-unit conversion in either direction multiplies or divides by certified pi,
with a 4096-bit precision ceiling. Same-unit RationalPi expressions cancel pi
symbolically. Validate the Result path and numerical profile independently for each change. See the [family contract](CRV-06_color_ramp.md#maintained-implementation-and-validation)
for the shared behavior and acceptance requirements.
