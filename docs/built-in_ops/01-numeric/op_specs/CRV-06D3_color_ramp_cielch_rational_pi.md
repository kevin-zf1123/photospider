---
spec_schema_version: 1
id: CRV-06D3
parent_id: CRV-06D
function: color_ramp_cielch_rational_pi
operation_family: curve.color_ramp_cielch_rational_pi
proposed_operation_keys:
  - curve.color_ramp_cielch_rational_pi_strict
  - curve.color_ramp_cielch_rational_pi_accelerated_apple_silicon
  - curve.color_ramp_cielch_rational_pi_accelerated_x86_64
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

# CRV-06D3: color_ramp_cielch_rational_pi

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

## Interface and purpose

This independent CIELCh(ab) ramp uses exact rational pi-multiple source hue.
The ordered dynamic inputs are input, stops, lightness_chroma, hue_numerator, hue_denominator; output is named values.
lightness_chroma is Float32/Float64[K,2]; numerator and denominator are Int64[K], denominator>0, and unreduced fractions are valid.
input and stops independently accept Float32/Float64. stops is [K] with
1<=K<=65536; input has rank 1..7, and values has shape input.shape+[3].
All logical element limits, matching K and finite-value rules come from the
[complete CIELCh contract](CRV-06D_color_ramp_cielch.md).

Static String parameters are color_description, dtype, out_of_domain
and output_hue_unit. Constructors default dtype to lightness_chroma dtype,
out_of_domain to clamp, output_hue_unit to pi_multiple.
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
Whole collection and one/two selected-row mathematics, descriptor matching, whole-input dirty support,
immutable arbitrary-stride input, packed output mapping, owners beyond context
lifetime, capacity/work/stage accounting, cancellation and cache-off behavior.
All numerator and denominator values are collected; unused generic q<=0 is mathematically ignored, while selected q<=0 fails even if C=0.
Failure categories and complete-color publication follow the shared contract,
including ResourceExhausted on unfinished certified arithmetic.

## Workflow and acceptance

Bind the ordered ports above, stops=[0,1], matching color_description, and
explicit static parameters. lightness_chroma=[[0.25,2],[0.75,4]], hue_numerator=[7,1], hue_denominator=[4,4], input=[0.5] gives [[0.5,3,1]].
The example uses the default output unit and a described values result.
The maintained public Compiler/ExecutionContext workflow and invocation commands are linked below.

Use the shared independent arithmetic oracle, per-profile/dtype/unit fixtures,
multi-turn and large-angle cases, whole versus partial color requests, whole-input dirty
witnesses, invalid remote rows, strides, low-budget failures, cancellation and
owner-lifetime acceptance. Include INT64_MIN and equivalent unreduced fractions.
The maintained public workflow and current validation boundary are documented below; no performance result is claimed.

## Maintained implementation and validation

Public [`color_ramp_cielch_rational_pi_node`](../../../../include/photospider/numeric/color_ramps.hpp)
constructs this primitive; [`color_ramps.cpp`](../../../../plugins/ops/01-numeric/color_ramps.cpp)
implements its Whole complete-output execution.

Coordinates and original hue ratios use exact rational interpolation.
Conversion between radian and pi units uses a certified pi enclosure with
a 4096-bit precision ceiling; equal units cancel symbolically. All profiles
return strict bits. Unresolved unit conversion returns ResourceExhausted.

See the [family implementation](CRV-06_color_ramp.md#maintained-implementation-and-validation),
[mathematical machinery](../math-implementation.md#crv-06-colorarray-and-color-ramps)
and [public workflow commands](../../../../examples/numeric_workflow/README.md#color-ramps)
for resource limits and actual validation.
