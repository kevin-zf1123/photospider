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
implementation_status: implemented_subset
clarification_status: complete
---

# CRV-06D1: color_ramp_cielch

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
and no alpha; output metadata records the selected floating hue unit.

## Inherited executable contract

All original-hue interpolation, C=0 hue retention, correct final
rounding and unit-conversion rules are normative from CRV-06D.
Strict is correctly rounded; accelerated finite arithmetic follows the shared
FP32-scaled final-result bound. No GPU variant is specified.
Exact hit/clamp directly converts the original hue; no source hue is discarded
because chroma is zero. A mismatched platform is not silently redirected.

Inherit CRV-06D's complete-color observation expansion, global stops validation,
Whole Result validation and one/two selected-row mathematics, descriptor matching, whole-input dirty support,
authorized source windows, full-shape transactional output, owner lifetime,
capacity/work/stage accounting and cancellation are inherited from the family contract.
Unused generic hue is mathematically ignored; complete typed validation still applies; a selected nonfinite hue fails even if C=0.
Failure categories and complete-color publication follow the shared contract,
including ResourceExhausted on unfinished certified arithmetic.

## Workflow and acceptance

Bind the ordered ports above, stops=[0,1], matching color_description, and
explicit static parameters. colors=[[0.25,2,0],[0.75,4,0]], input=[0.5] gives [[0.5,3,+0]].
The example uses the default output unit and a described values result.
The maintained public Compiler/ExecutionContext workflow and invocation commands are linked below.

Use the shared independent arithmetic oracle, per-profile/dtype/unit fixtures,
multi-turn and large-angle cases, whole versus partial color requests, whole-input dirty
witnesses, invalid remote rows, strides, low-budget failures, cancellation and
owner-lifetime acceptance. Include source/destination precision mixing and finite huge hues.
The maintained public workflow and current validation boundary are documented below; no performance result is claimed.

## Maintained implementation and validation

The public helper `color_ramp_cielch_node` is declared in
[`color_ramps.hpp`](../../../../plugins/ops/include/photospider/ops/numeric/color_ramps.hpp).
`color_ramps.cpp` prepares immutable model/unit/profile state and executes the
Whole Result operation. Original hue values use exact rational interpolation;
Cross-unit conversion in either direction multiplies or divides by certified pi,
with a 4096-bit precision ceiling. Same-unit RationalPi expressions cancel pi
symbolically. Focused Result CTest, Strict/Apple manual groups, independent numerical oracles
and installed-consumer checks pass. See the [family contract](CRV-06_color_ramp.md#maintained-implementation-and-validation)
and [workflow README](../../../../examples/numeric_workflow/README.md#color-ramps)
for evidence scope and untested platforms/shapes.
