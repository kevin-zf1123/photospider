---
spec_schema_version: 1
id: CRV-06G3
parent_id: CRV-06G
function: color_ramp_hsl_rational_pi
operation_family: curve.color_ramp_hsl_rational_pi
proposed_operation_keys:
  - curve.color_ramp_hsl_rational_pi_strict
  - curve.color_ramp_hsl_rational_pi_accelerated_apple_silicon
  - curve.color_ramp_hsl_rational_pi_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06G3: color_ramp_hsl_rational_pi

The five dynamic ports inherit the family sole-tensor Result rule: each has
exactly one tensor member and no fields under any structurally valid schema id,
version and member key. Use complete `sample_shape()` values including batch axes:
`input` is S, `stops` is [K], the model-specific color pair array is [K,2], and
`hue_numerator` and `hue_denominator` are [K].

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and purpose

This independent HSL ramp uses exact rational pi-multiple source hue.
The ordered dynamic inputs are input, stops, saturation_lightness, hue_numerator, hue_denominator; output is named values.
saturation_lightness is Float32/Float64[K,2]; numerator and denominator are Int64[K]; selected rows require denominator>0, and fractions need not be reduced. Unselected q<=0 has no added mathematical-domain check.
input and stops independently accept Float32/Float64. stops is [K] with
1<=K<=65536; input has rank 1..7, and values has shape sample_shape(input)+[3].
All logical element limits, matching K and finite-value rules come from the
[complete HSL contract](CRV-06G_color_ramp_hsl.md).

Static String parameters are color_description, dtype, out_of_domain
and output_hue_unit. Constructors default dtype to saturation_lightness dtype,
out_of_domain to clamp, output_hue_unit to pi_multiple.
Both radian and pi_multiple output units remain available. color_description
explicitly identifies the input representation, HSL, underlying RGB primaries/white/transfer (default sRGB/D65/sRGB)
and no alpha; output metadata records the selected floating hue unit.

## Inherited executable contract

All original-hue interpolation, S=0 hue retention, correct final
rounding and unit-conversion rules are normative from CRV-06G.
Strict is correctly rounded; accelerated finite arithmetic follows the shared
FP32-scaled final-result bound. No GPU variant is specified.
Exact hit/clamp directly converts the original hue; no source hue is discarded
because saturation is zero. A mismatched platform is not silently redirected.

Inherit CRV-06G's complete-color observation expansion, global stops validation,
Whole Result validation and one/two selected-row mathematics, descriptor matching, whole-input dirty support,
authorized source windows, full-shape transactional output, owner lifetime,
capacity/work/stage accounting and cancellation are inherited from the family contract.
Complete typed/upstream validation covers all numerator and denominator inputs; unused q<=0 rows receive no additional mathematical-domain check, while a selected q<=0 fails even if S=0.
Failure categories and complete-color publication follow the shared contract,
including ResourceExhausted on unfinished certified arithmetic.

## Workflow and acceptance

Bind the ordered ports above, stops=[0,1], matching color_description, and
explicit static parameters. saturation_lightness=[[0.25,0.125],[0.75,0.875]], hue_numerator=[0,4], hue_denominator=[1,1], input=[0.5] gives [[2,0.5,0.5]].
The example uses the default output unit and a described values result.
The maintained public Compiler/ExecutionContext workflow and invocation commands are linked below.

Use the shared independent arithmetic oracle, per-profile/dtype/unit fixtures,
multi-turn and large-angle cases, whole versus partial color requests, whole-input dirty
witnesses, invalid remote rows, strides, low-budget failures, cancellation and
owner-lifetime acceptance. Include INT64_MIN and equivalent unreduced fractions.
The maintained public workflow and current validation boundary are documented below; no performance result is claimed.

## Maintained implementation and validation

The public helper `color_ramp_hsl_rational_pi_node` is declared in
[`color_ramps.hpp`](../../../../plugins/ops/include/photospider/ops/numeric/color_ramps.hpp).
`color_ramps.cpp` prepares immutable model/unit/profile state and executes the
Whole Result operation. Original hue values use exact rational interpolation;
Cross-unit conversion in either direction multiplies or divides by certified pi,
with a 4096-bit precision ceiling. Same-unit RationalPi expressions cancel pi
symbolically. Focused Result CTest, Strict/Apple manual groups, independent numerical oracles
and installed-consumer checks pass. See the [family contract](CRV-06_color_ramp.md#maintained-implementation-and-validation)
and [workflow README](../../../../examples/numeric_workflow/README.md#color-ramps)
for evidence scope and untested platforms/shapes.
