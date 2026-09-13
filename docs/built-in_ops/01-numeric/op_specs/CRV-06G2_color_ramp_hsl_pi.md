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
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-06G2: color_ramp_hsl_pi

## Interface and purpose

This independent HSL ramp uses floating pi-multiple source hue.
The ordered dynamic inputs are input, stops, colors; output is named values.
colors is Float32/Float64[K,3], ordered H, S, L.
input and stops independently accept Float32/Float64. stops is [K] with
1<=K<=65536; input has rank 1..7, and values has shape input.shape+[3].
All logical element limits, matching K and finite-value rules come from the
[complete HSL contract](CRV-06G_color_ramp_hsl.md).

Static String parameters are color_description, dtype, out_of_domain
and output_hue_unit. Constructors default dtype to colors dtype,
out_of_domain to clamp, output_hue_unit to pi_multiple.
Both radian and pi_multiple output units remain available. color_description
explicitly identifies the input representation, HSL, underlying RGB primaries/white/transfer (default sRGB/D65/sRGB)
and no alpha; output metadata records the selected floating hue unit.

## Inherited executable contract

All original-hue interpolation, S=0 hue retention, correct final
rounding and unit-conversion rules are normative from CRV-06G.
All three CPU versions are bitwise identical. No GPU variant is specified.
Exact hit/clamp directly converts the original hue; no source hue is discarded
because saturation is zero. A mismatched platform is not silently redirected.

Inherit CRV-06G's complete-color observation expansion, global stops validation,
one/two selected-row demand, descriptor matching, exact dirty support,
immutable arbitrary-stride input, packed output mapping, owners beyond context
lifetime, capacity/work/stage accounting, cancellation and cache-off behavior.
Nonfinite hue in an unselected row is not observed; a selected nonfinite hue fails even if S=0.
Failure categories and complete-color publication follow the shared contract,
including ResourceExhausted on unfinished certified arithmetic.

## Conceptual workflow and acceptance

Bind the ordered ports above, stops=[0,1], matching color_description, and
explicit static parameters. colors=[[0,0.25,0.125],[4,0.75,0.875]], input=[0.5] gives [[2,0.5,0.5]].
The example uses the default output unit and a described values result.
This is a conceptual Compiler/ExecutionContext workflow until registration and
ColorArray support are implemented; delivery must add actual invocation commands.

Use the shared independent arithmetic oracle, per-profile/dtype/unit fixtures,
multi-turn and large-angle cases, whole versus partial color requests, exact dirty
witnesses, invalid remote rows, strides, low-budget failures, cancellation and
owner-lifetime acceptance. Include source/destination precision mixing and finite huge hues.
No runtime execution or numerical compatibility claim accompanies this Proposed
specification.
