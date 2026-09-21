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
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-06D1: color_ramp_cielch

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and purpose

This independent CIELCh(ab) ramp uses floating radian source hue.
The ordered dynamic inputs are input, stops, colors; output is named values.
colors is Float32/Float64[K,3], ordered L*, C*, h.
input and stops independently accept Float32/Float64. stops is [K] with
1<=K<=65536; input has rank 1..7, and values has shape input.shape+[3].
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
Whole collection and one/two selected-row mathematics, descriptor matching, whole-input dirty support,
immutable arbitrary-stride input, packed output mapping, owners beyond context
lifetime, capacity/work/stage accounting, cancellation and cache-off behavior.
Unused generic hue is mathematically ignored; complete typed validation still applies; a selected nonfinite hue fails even if C=0.
Failure categories and complete-color publication follow the shared contract,
including ResourceExhausted on unfinished certified arithmetic.

## Workflow and acceptance

Bind the ordered ports above, stops=[0,1], matching color_description, and
explicit static parameters. colors=[[20,2,0],[80,4,0]], input=[0.5] gives [[50,3,+0]].
The example uses the default output unit and a described values result.
The maintained public Compiler/ExecutionContext workflow and invocation commands are linked below.

Use the shared independent arithmetic oracle, per-profile/dtype/unit fixtures,
multi-turn and large-angle cases, whole versus partial color requests, whole-input dirty
witnesses, invalid remote rows, strides, low-budget failures, cancellation and
owner-lifetime acceptance. Include source/destination precision mixing and finite huge hues.
The maintained public workflow and current validation boundary are documented below; no performance result is claimed.

## Maintained implementation and validation

Public [`color_ramp_cielch_node`](../../../../include/photospider/numeric/color_ramps.hpp)
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
